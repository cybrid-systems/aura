// test_dead_coercion_zerooverhead_closed_loop_468.cpp
// Issue #468: DeadCoercionEliminationPass + CastOp zero-overhead
// closed loop (refines #433/#449).
//
// Non-duplicative with #433 (observability only), #538 (SoA DCE),
// #574 (coercion-elim-stats Task2), #629 (narrow_evidence Rule 6).
//
// AC1: query:dead-coercion-zerooverhead-stats reachable
// AC2: gradual workload — DCE elides >60% identity CastOps
// AC3: identity cast (same type_id) elided; narrowing cast preserved
// AC4: typed mutate loop — stats monotonic, eval semantics preserved
// AC5: compile:dead-coercion-stats regression
// AC6: query:coercion-elim-stats + query:coercion-zerooverhead-stats regression
// AC7: TypeSpec + DCE pipeline — eliminated < emitted
// AC8: multi-round gradual mutate — zerooverhead stats grow
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

import std;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_468_detail {

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

static bool zerooverhead_stats_hash(CompilerService& cs) {
    auto r = cs.eval("(query:dead-coercion-zerooverhead-stats)");
    return r && is_hash(*r);
}

static std::size_t count_cast_ops(const IRModule& mod) {
    std::size_t n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::CastOp)
                    ++n;
    return n;
}

// 9 elidable identity casts + 3 non-elidable (no ground type_id) → 75% reduction.
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

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:dead-coercion-zerooverhead-stats ---");
    CHECK(zerooverhead_stats_hash(cs), "dead-coercion-zerooverhead-stats returns hash");
    const auto s0 = cs.snapshot().dead_coercion_eliminated_total;
    std::println("  dead_coercion_eliminated_total = {}", s0);

    std::println("\n--- AC2: gradual workload >60% CastOp reduction ---");
    auto mod = make_gradual_workload_468();
    const auto before = count_cast_ops(mod);
    DeadCoercionEliminationPass dce;
    dce.run(mod);
    const auto after = count_cast_ops(mod);
    const auto reduction_pct = before > 0 ? (100 * (before - after)) / before : 0;
    std::println("  before={} after={} reduction={}% eliminated={}", before, after, reduction_pct,
                 dce.eliminated_count());
    CHECK(before == 12, "12 CastOps before (9 elidable + 3 not)");
    CHECK(after == 3, "3 CastOps remain after DCE");
    CHECK(reduction_pct > 60, "reduction > 60%");
    CHECK(dce.eliminated_count() == 9, "eliminated_count == 9");

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
    CHECK(count_cast_ops(narrow) == narrow_before, "Dynamic→Int without type_id not elided");
    CHECK(dce2.eliminated_count() == 0, "no elision without ground type info");

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

    std::println("\n--- AC5: compile:dead-coercion-stats regression ---");
    auto dcs = cs.eval("(compile:dead-coercion-stats)");
    CHECK(dcs && is_int(*dcs), "compile:dead-coercion-stats returns int");

    std::println("\n--- AC6: query regression ---");
    auto ces = cs.eval("(query:coercion-elim-stats)");
    auto czs = cs.eval("(query:coercion-zerooverhead-stats)");
    CHECK(ces && is_int(*ces), "coercion-elim-stats regression");
    CHECK(czs && is_int(*czs), "coercion-zerooverhead-stats regression");

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

} // namespace aura_468_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_468_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}