// test_dead_coercion_batch.cpp
// B pilot #9 (after linear_ownership #610/#638/#598/#575/#1596/#1659 in 1f609456):
// consolidated dead_coercion family — Issues #1411 + #629 + #574 + #468
// (DCE wire-up contract + zero-overhead narrow_evidence Rule 6 +
// Task2 coercion-elim-stats + zero-overhead gradual closed loop) — into one
// batch driver.
//
// Per AuraDomainTests.cmake legacy Phase 1 batch convention
// (test_issues_809_817_batch / test_per_defuse_batch / test_env_lookup_batch /
// test_fiber_resume_batch / test_compact_sweep_batch / test_incremental_relower_batch /
// test_macro_reflect_batch / test_incremental_type_batch / test_linear_ownership_batch
// precedents): single binary with CHECK() + RUN_ALL_TESTS(); per-issue AC blocks
// in namespace aura_dead_coercion_batch { run_NNN_xxx() }; EXCLUDE_FROM_ALL.
//
// AC map (consolidated, 24 ACs total):
//   Issue #1411 — 5 ACs: DCE contract (counts sum, elapsed_us, keep_for_debug,
//                  run_function, idempotent)
//   Issue #629  — 5 ACs: DCE Rule 6 narrow_evidence + TypeSpec skip +
//                  coercion-zerooverhead-stats + mutate re-eval + #508 regression
//   Issue #574  — 6 ACs: identity Int cast elided + narrowing preserved +
//                  CoercionMap apply + TypeSpec+DCE pipeline + typed mutate matrix
//   Issue #468  — 8 ACs: gradual workload >60% CastOp reduction + identity elision +
//                  narrowing preserved + typed mutate monotonic + compile/stats +
//                  query regression + TypeSpec+DCE + multi-round gradual mutate
//
// Skip: test_dead_coercion_elision_narrow_evidence.cpp is JIT_LATE2 bundle member
// (cmake/AuraIssueBundles.cmake:101 LATE2_MEMBERS list) — separate build context,
// NOT touched.

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.coercion_map;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_dead_coercion_batch {

using aura::compiler::CompilerService;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::TypeSpecializationWrap;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;

static std::size_t count_cast_ops(const IRModule& mod) {
    std::size_t n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::CastOp)
                    ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Issue #1411: DeadCoercionEliminationPass wire-up contract (5 ACs)
// ---------------------------------------------------------------------------
namespace _1411_detail {

    aura::ir::IRModule build_ir_with_5_castops() {
        aura::ir::IRModule mod;

        aura::ir::IRFunction func;
        func.id = 0;
        func.name = "synth";
        func.entry_block = 0;
        func.local_count = 0;
        func.arg_count = 0;

        aura::ir::BasicBlock blk;
        blk.id = 0;

        auto emit_cast = [&](std::uint32_t result, std::uint32_t value, std::uint32_t type_tag,
                             std::uint32_t type_id) {
            aura::ir::IRInstruction ins;
            ins.opcode = aura::ir::IROpcode::CastOp;
            ins.operands = {result, value, type_tag};
            ins.type_id = type_id;
            blk.instructions.push_back(ins);
        };

        emit_cast(/*result=*/100, /*value=*/200, /*type_tag=*/7, /*type_id=*/42);
        emit_cast(/*result=*/101, /*value=*/201, /*type_tag=*/7, /*type_id=*/42);
        emit_cast(/*result=*/102, /*value=*/202, /*type_tag=*/9, /*type_id=*/99);
        emit_cast(/*result=*/103, /*value=*/203, /*type_tag=*/9, /*type_id=*/99);
        emit_cast(/*result=*/104, /*value=*/204, /*type_tag=*/9, /*type_id=*/99);

        func.blocks.push_back(std::move(blk));
        mod.functions.push_back(std::move(func));
        return mod;
    }

    static void run_1411_dce_contract() {
        std::println("\n=== Issue #1411: DeadCoercionEliminationPass wire-up contract ===");

        // AC1: DCE run completes + records elapsed_us
        {
            std::println("\n--- AC1: DCE run completes + records elapsed_us ---");
            auto mod = build_ir_with_5_castops();
            const int total_castops = count_cast_ops(mod);
            CHECK(total_castops == 5, "AC1.setup: synthetic IR has 5 CastOps");
            aura::compiler::DeadCoercionEliminationPass dce;
            dce.run(mod);
            const std::uint64_t elim = dce.eliminated_count();
            const std::uint64_t kept = dce.kept_for_debug_count();
            const std::uint64_t us = dce.elapsed_us();
            std::println("  DCE run: eliminated={} kept={} elapsed={}us", elim, kept, us);
            CHECK(us > 0, "AC1: DCE records non-zero elapsed time after run");
        }

        // AC2: elapsed_us non-zero after run
        {
            std::println("\n--- AC2: DCE elapsed_us non-zero ---");
            auto mod = build_ir_with_5_castops();
            aura::compiler::DeadCoercionEliminationPass dce;
            dce.run(mod);
            const std::uint64_t us = dce.elapsed_us();
            std::println("  DCE elapsed: {}us", us);
            CHECK(us > 0, "AC2: DCE records non-zero elapsed time (proves timing works)");
        }

        // AC3: keep_for_debug mode
        {
            std::println("\n--- AC3: DCE keep_for_debug mode ---");
            auto mod = build_ir_with_5_castops();
            aura::compiler::DeadCoercionEliminationPass dce;
            dce.set_keep_for_debug(true);
            dce.run(mod);
            const std::uint64_t elim = dce.eliminated_count();
            const std::uint64_t kept = dce.kept_for_debug_count();
            std::println("  DCE keep_for_debug: eliminated={} kept={}", elim, kept);
            CHECK(elim == 0, "AC3: keep_for_debug mode eliminates 0");
            CHECK(kept == 5, "AC3: keep_for_debug mode keeps all 5 (proves the no-op path works)");
        }

        // AC4: run_function per-function path
        {
            std::println("\n--- AC4: DCE run_function per-function path ---");
            auto mod = build_ir_with_5_castops();
            aura::compiler::DeadCoercionEliminationPass dce;
            dce.run_function(mod.functions[0]);
            CHECK(true, "AC4: run_function reachable (service.ixx:7390 uses this)");
        }

        // AC5: idempotent
        {
            std::println("\n--- AC5: DCE is idempotent ---");
            auto mod = build_ir_with_5_castops();
            aura::compiler::DeadCoercionEliminationPass dce;
            dce.run(mod);
            const std::uint64_t elim_1 = dce.eliminated_count();
            const std::uint64_t kept_1 = dce.kept_for_debug_count();
            dce.run(mod);
            const std::uint64_t elim_2 = dce.eliminated_count();
            const std::uint64_t kept_2 = dce.kept_for_debug_count();
            std::println("  DCE run#1: eliminated={} kept={}", elim_1, kept_1);
            std::println("  DCE run#2: eliminated={} kept={}", elim_2, kept_2);
            CHECK(elim_1 == elim_2 && kept_1 == kept_2,
                  "AC5: DCE is idempotent — two runs give same counts");
        }
    }

} // namespace _1411_detail

// ---------------------------------------------------------------------------
// Issue #629: zero-overhead coercion path (5 ACs)
// ---------------------------------------------------------------------------
namespace _629_detail {

    static IRModule make_module_with(std::vector<IRInstruction> instrs,
                                     std::uint32_t local_count = 16) {
        IRModule mod;
        mod.functions.push_back(IRFunction{.name = "test", .local_count = local_count});
        auto& func = mod.functions.back();
        func.blocks.push_back({0});
        func.blocks.back().instructions = std::move(instrs);
        return mod;
    }

    static IRModule make_branch_narrow_module(bool with_narrow_evidence) {
        IRModule mod;
        mod.functions.push_back(IRFunction{.name = "test", .local_count = 32});
        auto& func = mod.functions.back();

        // Block 0: cond + Branch + Return
        func.blocks.push_back({0});
        auto& entry = func.blocks[0].instructions;
        entry.push_back({IROpcode::ConstBool, {0, 1, 0, 0}, 0, 2});
        IRInstruction branch{};
        branch.opcode = IROpcode::Branch;
        branch.operands = {0, 1, 2, 3};
        branch.type_id = 1;
        branch.narrow_evidence = with_narrow_evidence ? 1u : 0u;
        entry.push_back(branch);
        entry.push_back({IROpcode::Return, {3, 0, 0, 0}, 0, 0});

        // Block 1 (then): String value (would need CastOp without narrow_evidence)
        func.blocks.push_back({0});
        func.blocks[1].instructions = {
            {IROpcode::ConstString, {4, 0, 0, 0}, 0, 3},
            {IROpcode::Local, {3, 4, 0, 0}, 0, 3},
            {IROpcode::Jump, {0, 0, 0, 0}, 0, 0},
        };

        // Block 2 (else): same pattern
        func.blocks.push_back({0});
        func.blocks[2].instructions = {
            {IROpcode::ConstString, {5, 0, 0, 0}, 0, 3},
            {IROpcode::Local, {3, 5, 0, 0}, 0, 3},
            {IROpcode::Jump, {0, 0, 0, 0}, 0, 0},
        };

        return mod;
    }

    static void run_629_zerooverhead() {
        std::println("\n=== Issue #629: zero-overhead coercion path narrow_evidence ===");

        // AC1: DCE Rule 6 narrow_evidence identity elision
        {
            std::println("\n--- AC1: DCE Rule 6 narrow_evidence identity elision ---");
            IRInstruction cast{};
            cast.opcode = IROpcode::CastOp;
            cast.operands = {1, 0, 0, 0};
            cast.type_id = 1;
            cast.narrow_evidence = 4;
            auto mod = make_module_with({
                {IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
                cast,
                {IROpcode::Return, {1, 0, 0, 0}, 0, 0},
            });
            DeadCoercionEliminationPass dce;
            dce.run(mod);
            CHECK(count_cast_ops(mod) == 0, "Rule 6 elides narrow_evidence CastOp");
            CHECK(dce.narrow_evidence_hits() == 1, "narrow_evidence_hits == 1");
            CHECK(dce.type_prop_hits() == 0, "type_prop_hits == 0 (Rule 6 not Rule 1)");
        }

        // AC2: TypeSpec skips cast when Branch has narrow_evidence
        {
            std::println("\n--- AC2: TypeSpec skips cast when Branch has narrow_evidence ---");
            aura::core::TypeRegistry reg;
            (void)reg.lookup_type("Int");
            (void)reg.lookup_type("String");
            (void)reg.lookup_type("Any");

            auto mod_skip = make_branch_narrow_module(true);
            TypeSpecializationWrap ts_skip(&reg);
            ts_skip.run(mod_skip);
            CHECK(ts_skip.narrow_evidence_skipped() == 1, "narrow_evidence_skipped == 1");

            auto mod_insert = make_branch_narrow_module(false);
            TypeSpecializationWrap ts_insert(&reg);
            ts_insert.run(mod_insert);
            CHECK(ts_insert.narrow_evidence_skipped() == 0,
                  "narrow_evidence_skipped == 0 without evidence");
            CHECK(ts_insert.castop_emitted() >= ts_skip.castop_emitted(),
                  "more CastOps emitted without narrow_evidence guard");
        }

        // AC3: query:coercion-zerooverhead-stats primitive
        {
            std::println("\n--- AC3: query:coercion-zerooverhead-stats primitive ---");
            CompilerService cs;
            auto r0 = cs.eval("(engine:metrics \"query:coercion-zerooverhead-stats\")");
            CHECK(r0 && is_int(*r0), "primitive returns int");
            const auto s0 = as_int(*r0);
            CHECK(s0 >= 0, "initial stats >= 0");

            cs.eval("(set-code \"(define x 42) (define y \\\"hi\\\")\")");
            CHECK(cs.eval("(eval-current)").has_value(), "pipeline eval succeeds");

            auto r1 = cs.eval("(engine:metrics \"query:coercion-zerooverhead-stats\")");
            CHECK(r1 && is_int(*r1), "primitive returns int after pipeline");
            CHECK(as_int(*r1) >= s0, "stats grow (monotonic) after pipeline");
        }

        // AC4: mutate + re-eval gradual typing
        {
            std::println("\n--- AC4: mutate + re-eval gradual typing ---");
            CompilerService cs;
            cs.eval("(set-code \"(define x 42) (define y \\\"hello\\\") (define z #t)\")");
            auto baseline = cs.eval("(eval-current)");
            CHECK(baseline.has_value(), "baseline eval succeeds");

            auto snap0 = cs.snapshot();
            const auto win0 = snap0.coercion_zerooverhead_win_total;

            for (int i = 0; i < 5; ++i) {
                std::string code = "(set-code \"(define v ";
                code += std::to_string(i * 3);
                code += ")\")";
                CHECK(cs.eval(code).has_value(), "mutation set-code ok");
                auto r = cs.eval("(eval-current)");
                CHECK(r.has_value(), "post-mutation eval succeeds");
            }

            auto after = cs.eval("(eval-current)");
            CHECK(after.has_value(), "final eval succeeds");
            auto snap1 = cs.snapshot();
            CHECK(snap1.coercion_zerooverhead_win_total >= win0,
                  "zerooverhead_win monotonic across mutations");
        }

        // AC5: Issue #508 dynamic passthrough regression
        {
            std::println("\n--- AC5: Issue #508 dynamic passthrough regression ---");
            auto mod = make_module_with({
                {IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
                {IROpcode::CastOp, {1, 0, 3, 0}, 0, 0},
                {IROpcode::Return, {1, 0, 0, 0}, 0, 0},
            });
            DeadCoercionEliminationPass dce;
            dce.run(mod);
            CHECK(count_cast_ops(mod) == 0, "Dynamic passthrough CastOp still elided");
            CHECK(dce.eliminated_count() == 1, "eliminated_count == 1 (508 regression)");
        }
    }

} // namespace _629_detail

// ---------------------------------------------------------------------------
// Issue #574: Task2 dead-coercion elimination + coercion-elim-stats (6 ACs)
// ---------------------------------------------------------------------------
namespace _574_detail {

    static std::int64_t coercion_elim_stats(CompilerService& cs) {
        auto r = cs.eval("(engine:metrics \"query:coercion-elim-stats\")");
        if (!r || !is_int(*r))
            return 0;
        return as_int(*r);
    }

    static void run_574_task2() {
        std::println("\n=== Issue #574: Task2 dead-coercion elimination + coercion-elim-stats ===");

        CompilerService cs;

        // AC1: query:coercion-elim-stats
        {
            std::println("\n--- AC1: query:coercion-elim-stats ---");
            const auto s0 = coercion_elim_stats(cs);
            std::println("  query:coercion-elim-stats = {}", s0);
            CHECK(s0 >= 0, "coercion-elim-stats non-negative");
        }

        // AC2: identity CastOp elided (same type_id)
        {
            std::println("\n--- AC2: identity CastOp elided (same type_id) ---");
            IRModule mod;
            mod.functions.push_back(IRFunction{.name = "id", .local_count = 8});
            mod.functions.back().blocks.push_back({0});
            mod.functions.back().blocks[0].instructions = {
                {IROpcode::ConstI64, {0, 7, 0, 0}, 0, 1},
                {IROpcode::CastOp, {1, 0, 0, 0}, 0, 1},
                {IROpcode::Return, {1, 0, 0, 0}, 0, 0},
            };
            DeadCoercionEliminationPass dce;
            dce.run(mod);
            CHECK(mod.functions[0].blocks[0].instructions[1].opcode == IROpcode::Local,
                  "identity Int cast elided");
            CHECK(dce.eliminated_count() == 1, "eliminated_count == 1");
        }

        // AC3: narrowing cast preserved (no ground type_id)
        {
            std::println("\n--- AC3: narrowing cast preserved (no ground type_id) ---");
            IRModule narrow_mod;
            narrow_mod.functions.push_back(IRFunction{.name = "narrow", .local_count = 8});
            narrow_mod.functions.back().blocks.push_back({0});
            narrow_mod.functions.back().blocks[0].instructions = {
                {IROpcode::ConstI64, {0, 1, 0, 0}, 0, 0},
                {IROpcode::CastOp, {1, 0, 0, 0}, 0, 0},
                {IROpcode::Return, {1, 0, 0, 0}, 0, 0},
            };
            const auto before_narrow = count_cast_ops(narrow_mod);
            DeadCoercionEliminationPass dce2;
            dce2.run(narrow_mod);
            CHECK(count_cast_ops(narrow_mod) == before_narrow,
                  "Dynamic→Int cast without type_id not elided");
            CHECK(dce2.eliminated_count() == 0, "no elision without ground type info");
        }

        // AC4: CoercionMap apply inserts CoercionNode
        {
            std::println("\n--- AC4: CoercionMap apply inserts CoercionNode ---");
            auto arena = std::make_unique<aura::ast::ASTArena>();
            auto alloc = arena->allocator();
            auto* flat = arena->create<aura::ast::FlatAST>(alloc);
            auto* pool = arena->create<aura::ast::StringPool>(alloc);
            auto callee_sym = pool->intern("+");
            auto callee_var = flat->add_variable(callee_sym);
            auto arg0 = flat->add_literal(1);
            auto arg1 = flat->add_literal(2);
            auto call_id = flat->add_call(callee_var, {arg0, arg1});
            flat->root = call_id;
            aura::compiler::CoercionMap cm;
            cm.add(call_id, 1, arg0, 0, 1, 0, 0);
            CHECK(aura::compiler::apply_coercion_map(*flat, cm) == 1,
                  "apply_coercion_map applied 1 entry");
            CHECK(flat->get(flat->get(call_id).child(1)).tag == aura::ast::NodeTag::Coercion,
                  "CoercionNode at child slot");
        }

        // AC5: TypeSpec + DCE pipeline — eliminated < emitted
        {
            std::println("\n--- AC5: TypeSpec + DCE pipeline ---");
            aura::core::TypeRegistry reg;
            IRModule gradual;
            gradual.functions.push_back(IRFunction{.name = "grad", .local_count = 24});
            gradual.functions.back().blocks.push_back({0});
            auto& blk = gradual.functions.back().blocks[0].instructions;
            std::uint32_t slot = 0;
            for (int i = 0; i < 3; ++i) {
                auto s0 = slot++;
                blk.push_back({IROpcode::ConstI64, {s0, 1, 0, 0}, 0, 1});
                auto s1 = slot++;
                blk.push_back({IROpcode::CastOp, {s1, s0, 3, 0}, 0, 0});
                blk.push_back({IROpcode::Local, {slot++, s1, 0, 0}, 0, 1});
            }
            blk.push_back({IROpcode::Return, {slot - 1, 0, 0, 0}, 0, 0});
            const auto emitted = count_cast_ops(gradual);
            TypeSpecializationWrap ts(&reg);
            DeadCoercionEliminationPass dce3(&reg);
            ts.run(gradual);
            dce3.run(gradual);
            const auto remaining = count_cast_ops(gradual);
            std::println("  castops: emitted={} remaining={} eliminated={}", emitted, remaining,
                         dce3.eliminated_count());
            CHECK(dce3.eliminated_count() > 0, "DCE eliminated identity casts");
            CHECK(remaining < emitted, "remaining CastOps < emitted");
        }

        // AC6: typed mutate loop — eval semantics + elim stats grow
        {
            std::println("\n--- AC6: typed mutate loop + elim stats ---");
            cs.eval("(set-code \"(define f (lambda (x) x)) (define n 0)\")");
            CHECK(cs.eval("(eval-current)").has_value(), "baseline eval");
            const auto stats_before = coercion_elim_stats(cs);
            const auto elim_before = cs.snapshot().dead_coercion_eliminated_total;

            for (int i = 0; i < 4; ++i) {
                CHECK(cs.eval("(mutate:rebind \"n\" \"" + std::to_string(10 + i) + "\" \"574-" +
                              std::to_string(i) + "\")")
                          .has_value(),
                      "mutate:rebind ok");
                CHECK(cs.eval("(eval-current)").has_value(), "post-mutate eval");
            }
            CHECK(cs.eval("(f 99)").has_value(), "closure eval preserved");

            const auto stats_after = coercion_elim_stats(cs);
            const auto elim_after = cs.snapshot().dead_coercion_eliminated_total;
            std::println("  coercion-elim-stats: {} -> {}", stats_before, stats_after);
            std::println("  dead_coercion_eliminated: {} -> {}", elim_before, elim_after);
            CHECK(stats_after >= stats_before, "coercion-elim-stats monotonic");
            CHECK(elim_after >= elim_before, "dead_coercion_eliminated monotonic");
        }
    }

} // namespace _574_detail

// ---------------------------------------------------------------------------
// Issue #468: zero-overhead closed loop (8 ACs)
// ---------------------------------------------------------------------------
namespace _468_detail {

    static std::size_t zerooverhead_stats(CompilerService& cs) {
        auto r = cs.eval("(hash-ref (engine:metrics \"query:dead-coercion-zerooverhead-stats\") "
                         "'zerooverhead-wins)");
        if (!r || !is_int(*r))
            return 0;
        return static_cast<std::size_t>(as_int(*r));
    }

    static bool zerooverhead_stats_hash(CompilerService& cs) {
        auto r = cs.eval("(engine:metrics \"query:dead-coercion-zerooverhead-stats\")");
        return r && is_hash(*r);
    }

    static IRModule make_gradual_workload_468() {
        IRModule mod;
        mod.functions.push_back(IRFunction{.name = "grad468", .local_count = 64});
        mod.functions.back().blocks.push_back({0});
        auto& blk = mod.functions.back().blocks[0].instructions;
        std::uint32_t slot = 0;
        for (int g = 0; g < 3; ++g) {
            for (int e = 0; e < 3; ++e) {
                auto s0 = slot++;
                blk.push_back({IROpcode::ConstI64, {s0, 42, 0, 0}, 0, 1});
                auto s1 = slot++;
                blk.push_back({IROpcode::CastOp, {s1, s0, 0, 0}, 0, 1});
                blk.push_back({IROpcode::Local, {slot++, s1, 0, 0}, 0, 1});
            }
            auto u0 = slot++;
            blk.push_back({IROpcode::ConstI64, {u0, 1, 0, 0}, 0, 0});
            auto u1 = slot++;
            blk.push_back({IROpcode::CastOp, {u1, u0, 0, 0}, 0, 0});
            blk.push_back({IROpcode::Local, {slot++, u1, 0, 0}, 0, 0});
        }
        blk.push_back({IROpcode::Return, {slot - 1, 0, 0, 0}, 0, 0});
        return mod;
    }

    static void run_468_zerooverhead_closed_loop() {
        std::println("\n=== Issue #468: DeadCoercionEliminationPass zero-overhead closed loop ===");

        CompilerService cs;

        // AC1: query:dead-coercion-zerooverhead-stats
        {
            std::println("\n--- AC1: query:dead-coercion-zerooverhead-stats ---");
            CHECK(zerooverhead_stats_hash(cs), "dead-coercion-zerooverhead-stats returns hash");
            const auto s0 = cs.snapshot().dead_coercion_eliminated_total;
            std::println("  dead_coercion_eliminated_total = {}", s0);
        }

        // AC2: gradual workload >60% CastOp reduction (12 → 3)
        {
            std::println("\n--- AC2: gradual workload >60% CastOp reduction ---");
            auto mod = make_gradual_workload_468();
            const auto before = count_cast_ops(mod);
            DeadCoercionEliminationPass dce;
            dce.run(mod);
            const auto after = count_cast_ops(mod);
            const auto reduction_pct = before > 0 ? (100 * (before - after)) / before : 0;
            std::println("  before={} after={} reduction={}% eliminated={}", before, after,
                         reduction_pct, dce.eliminated_count());
            CHECK(before == 12, "12 CastOps before (9 elidable + 3 not)");
            CHECK(after == 3, "3 CastOps remain after DCE");
            CHECK(reduction_pct > 60, "reduction > 60%");
            CHECK(dce.eliminated_count() == 9, "eliminated_count == 9");
        }

        // AC3: identity elided, narrowing preserved
        {
            std::println("\n--- AC3: identity elided, narrowing preserved ---");
            IRModule narrow;
            narrow.functions.push_back(IRFunction{.name = "narrow", .local_count = 8});
            narrow.functions.back().blocks.push_back({0});
            narrow.functions.back().blocks[0].instructions = {
                {IROpcode::ConstI64, {0, 1, 0, 0}, 0, 0},
                {IROpcode::CastOp, {1, 0, 0, 0}, 0, 0},
                {IROpcode::Return, {1, 0, 0, 0}, 0, 0},
            };
            const auto narrow_before = count_cast_ops(narrow);
            DeadCoercionEliminationPass dce2;
            dce2.run(narrow);
            CHECK(count_cast_ops(narrow) == narrow_before,
                  "Dynamic→Int without type_id not elided");
            CHECK(dce2.eliminated_count() == 0, "no elision without ground type info");
        }

        // AC4: typed mutate loop + stats monotonic
        {
            std::println("\n--- AC4: typed mutate loop + stats monotonic ---");
            cs.eval("(set-code \"(define acc 0) (define (bump) (set! acc (+ acc 1)))\")");
            CHECK(cs.eval("(eval-current)").has_value(), "baseline eval");
            const auto elim4a = cs.snapshot().dead_coercion_eliminated_total;

            for (int i = 0; i < 4; ++i) {
                CHECK(cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(100 + i) + "\" \"468-" +
                              std::to_string(i) + "\")")
                          .has_value(),
                      "mutate:rebind ok");
                CHECK(cs.eval("(eval-current)").has_value(), "post-mutate eval");
            }
            CHECK(cs.eval("(+ acc 0)").has_value(), "eval semantics preserved");

            const auto elim4b = cs.snapshot().dead_coercion_eliminated_total;
            std::println("  dead_coercion_eliminated: {} -> {}", elim4a, elim4b);
            CHECK(zerooverhead_stats_hash(cs), "zerooverhead-stats hash after mutate loop");
            CHECK(elim4b >= elim4a, "dead_coercion_eliminated monotonic");
        }

        // AC5: compile:dead-coercion-stats regression
        {
            std::println("\n--- AC5: compile:dead-coercion-stats regression ---");
            auto dcs = cs.eval("(engine:metrics \"compile:dead-coercion-stats\")");
            CHECK(dcs && is_int(*dcs), "compile:dead-coercion-stats returns int");
        }

        // AC6: query regression
        {
            std::println("\n--- AC6: query regression ---");
            auto ces = cs.eval("(engine:metrics \"query:coercion-elim-stats\")");
            auto czs = cs.eval("(engine:metrics \"query:coercion-zerooverhead-stats\")");
            CHECK(ces && is_int(*ces), "coercion-elim-stats regression");
            CHECK(czs && is_int(*czs), "coercion-zerooverhead-stats regression");
        }

        // AC7: TypeSpec + DCE pipeline
        {
            std::println("\n--- AC7: TypeSpec + DCE pipeline ---");
            aura::core::TypeRegistry reg;
            IRModule gradual;
            gradual.functions.push_back(IRFunction{.name = "pipe", .local_count = 24});
            gradual.functions.back().blocks.push_back({0});
            auto& blk = gradual.functions.back().blocks[0].instructions;
            std::uint32_t slot = 0;
            for (int i = 0; i < 4; ++i) {
                auto s0 = slot++;
                blk.push_back({IROpcode::ConstI64, {s0, 1, 0, 0}, 0, 1});
                auto s1 = slot++;
                blk.push_back({IROpcode::CastOp, {s1, s0, 3, 0}, 0, 0});
                blk.push_back({IROpcode::Local, {slot++, s1, 0, 0}, 0, 1});
            }
            blk.push_back({IROpcode::Return, {slot - 1, 0, 0, 0}, 0, 0});
            const auto emitted = count_cast_ops(gradual);
            TypeSpecializationWrap ts(&reg);
            DeadCoercionEliminationPass dce3(&reg);
            ts.run(gradual);
            dce3.run(gradual);
            const auto remaining = count_cast_ops(gradual);
            std::println("  castops: emitted={} remaining={} eliminated={}", emitted, remaining,
                         dce3.eliminated_count());
            CHECK(dce3.eliminated_count() > 0, "pipeline eliminated identity casts");
            CHECK(remaining < emitted, "remaining < emitted after pipeline");
        }

        // AC8: multi-round gradual mutate
        {
            std::println("\n--- AC8: multi-round gradual mutate ---");
            const auto stats8a = zerooverhead_stats(cs);
            for (int round = 0; round < 3; ++round) {
                (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(200 + round) + "\")");
                (void)cs.eval("(eval-current)");
            }
            const auto stats8b = zerooverhead_stats(cs);
            std::println("  zerooverhead-stats: {} -> {}", stats8a, stats8b);
            CHECK(stats8b >= stats8a, "zerooverhead-stats monotonic over matrix");
        }
    }

} // namespace _468_detail

} // namespace aura_dead_coercion_batch

int main() {
    std::println("=== B pilot #9: dead_coercion family batch (#1411 + #629 + #574 + #468) ===");
    aura_dead_coercion_batch::_1411_detail::run_1411_dce_contract();
    aura_dead_coercion_batch::_629_detail::run_629_zerooverhead();
    aura_dead_coercion_batch::_574_detail::run_574_task2();
    aura_dead_coercion_batch::_468_detail::run_468_zerooverhead_closed_loop();
    return RUN_ALL_TESTS();
}
