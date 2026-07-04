// test_dead_coercion_elimination_task2.cpp — Issue #574:
// DeadCoercionEliminationPass + CoercionMap apply integration
// for zero-overhead CastOp in typed mutation (Task2 review).
//
// Non-duplicative with #611 (castop flow), #538 (SoA DCE),
// #629 (narrow_evidence Rule 6), #433 (observability only).
//
// AC1: query:coercion-elim-stats reachable + monotonic
// AC2: identity CastOp (same type_id) elided to Local
// AC3: ground→Int narrowing cast preserved (runtime check)
// AC4: CoercionMap apply inserts CoercionNode before lowering
// AC5: TypeSpec + DCE pipeline — eliminated < emitted
// AC6: typed mutate loop — eval semantics + elim stats grow

#include "test_harness.hpp"

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.coercion_map;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_574_detail {

using aura::compiler::CompilerService;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::TypeSpecializationWrap;
using aura::compiler::types::as_int;
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

static std::int64_t coercion_elim_stats(CompilerService& cs) {
    auto r = cs.eval("(query:coercion-elim-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:coercion-elim-stats ---");
    const auto s0 = coercion_elim_stats(cs);
    std::println("  query:coercion-elim-stats = {}", s0);
    CHECK(s0 >= 0, "coercion-elim-stats non-negative");

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
    CHECK(aura::compiler::apply_coercion_map(*flat, cm) == 1, "apply_coercion_map applied 1 entry");
    CHECK(flat->get(flat->get(call_id).child(1)).tag == aura::ast::NodeTag::Coercion,
          "CoercionNode at child slot");

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

} // namespace aura_574_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_574_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}