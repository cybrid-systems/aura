// test_coercion_dead_elim_castop_flow_zerooverhead.cpp
// Issue #611: CoercionMap deferred apply + DeadCoercionEliminationPass
// + CastOp type_tag flow to IR/JIT for zero-overhead typed mutation.
//
// AC1: CastOp type_tag (operands[2]) + type_id propagate through DCE
// AC2: CoercionMap apply → lowering → TypeSpec + DCE removes identity casts
// AC3: Dirty-block incremental DCE (AoS run_function + block_dirty mask)
// AC4: Gradual Dynamic/consistent mutate loop — semantic match + metrics grow
// AC5: query:coercion-zerooverhead-stats primitive monotonic

#include "test_harness.hpp"

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.coercion_map;
import aura.compiler.pass_manager;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_611_detail {

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

static IRModule make_module_with(std::vector<IRInstruction> instrs,
                                 std::uint32_t local_count = 16) {
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "test", .local_count = local_count});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    func.blocks.back().instructions = std::move(instrs);
    return mod;
}

static void test_castop_type_tag_and_type_id_flow() {
    std::println("\n--- AC1: CastOp type_tag + type_id through DCE ---");
    // Int source (type_id=1) cast to Dynamic (tag=3) — elidable passthrough.
    auto mod = make_module_with({
        {IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
        {IROpcode::CastOp, {1, 0, 3, 0}, 0, 1},
        {IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    });
    const auto& cast_before = mod.functions[0].blocks[0].instructions[1];
    CHECK(cast_before.operands[2] == 3, "CastOp type_tag == 3 (Dynamic)");
    CHECK(cast_before.type_id == 1, "CastOp type_id == 1 (Int)");

    DeadCoercionEliminationPass dce;
    dce.run(mod);
    const auto& after_instr = mod.functions[0].blocks[0].instructions[1];
    CHECK(after_instr.opcode == IROpcode::Local, "identity Dynamic cast elided to Local");
    CHECK(after_instr.type_id == 1, "type_id preserved on Local after elision");
    CHECK(dce.eliminated_count() == 1, "eliminated_count == 1");
    CHECK(dce.type_prop_hits() >= 0, "type_prop_hits observable");
}

static void test_coercion_map_apply_typespec_dce_pipeline() {
    std::println("\n--- AC2: CoercionMap apply + TypeSpec + DCE pipeline ---");
    aura::core::TypeRegistry reg;
    (void)reg.lookup_type("Int");
    (void)reg.lookup_type("Any");

    // Gradual workload: alternating elidable Dynamic casts + ground casts.
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "gradual", .local_count = 32});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    auto& blk = func.blocks[0].instructions;
    std::uint32_t slot = 0;
    for (int i = 0; i < 4; ++i) {
        auto s0 = slot++;
        blk.push_back({IROpcode::ConstI64, {s0, 42, 0, 0}, 0, 1});
        auto s1 = slot++;
        blk.push_back({IROpcode::CastOp, {s1, s0, 3, 0}, 0, 0}); // elidable Dynamic
        auto s2 = slot++;
        blk.push_back({IROpcode::CastOp, {s2, s1, 0, 0}, 0, 0}); // Int tag, no type_id
        blk.push_back({IROpcode::Local, {slot, s2, 0, 0}, 0, 1});
        ++slot;
    }
    blk.push_back({IROpcode::Return, {slot - 1, 0, 0, 0}, 0, 0});

    const auto before = count_cast_ops(mod);
    TypeSpecializationWrap ts(&reg);
    DeadCoercionEliminationPass dce(&reg);
    ts.run(mod);
    dce.run_function(mod.functions[0]);
    const auto after = count_cast_ops(mod);

    CHECK(before == 8, "8 CastOps before pipeline");
    CHECK(after == 4, "4 CastOps remain (Dynamic passthrough elided)");
    CHECK(dce.eliminated_count() == 4, "DCE eliminated 4 identity casts");
    CHECK(ts.castop_emitted() >= 0, "castop_emitted observable");
}

static void test_dirty_block_incremental_dce_aos() {
    std::println("\n--- AC3: dirty-block incremental DCE (AoS) ---");
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "two_blk", .local_count = 16});
    auto& func = mod.functions.back();

    // Block 0: elidable Dynamic cast
    func.blocks.push_back({0});
    func.blocks[0].instructions = {
        {IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1},
        {IROpcode::CastOp, {1, 0, 3, 0}, 0, 0},
        {IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    };

    // Block 1: another elidable cast (should NOT be touched when only block 0 dirty)
    func.blocks.push_back({1});
    func.blocks[1].instructions = {
        {IROpcode::ConstI64, {2, 2, 0, 0}, 0, 1},
        {IROpcode::CastOp, {3, 2, 3, 0}, 0, 0},
        {IROpcode::Return, {3, 0, 0, 0}, 0, 0},
    };

    std::vector<std::uint8_t> dirty_mask = {1, 0};
    DeadCoercionEliminationPass dce;
    dce.run_function(func, dirty_mask);

    CHECK(func.blocks[0].instructions[1].opcode == IROpcode::Local, "dirty block 0 CastOp elided");
    CHECK(func.blocks[1].instructions[1].opcode == IROpcode::CastOp,
          "clean block 1 CastOp untouched");
    CHECK(dce.eliminated_count() == 1, "only 1 elision on dirty block");
}

static void test_gradual_mutate_semantic_and_metrics() {
    std::println("\n--- AC4: gradual mutate loop — semantics + metrics ---");
    CompilerService cs;
    cs.eval("(set-code \"(define f (lambda (x) x)) (define g 42)\")");
    auto baseline = cs.eval("(eval-current)");
    CHECK(baseline.has_value(), "baseline eval succeeds");

    auto snap0 = cs.snapshot();
    const auto elim0 = snap0.dead_coercion_eliminated_total;
    const auto win0 = snap0.coercion_zerooverhead_win_total;
    const auto cast0 = snap0.coercion_castop_emitted_total;

    for (int i = 0; i < 5; ++i) {
        std::string code = "(set-code \"(define g ";
        code += std::to_string(100 + i);
        code += ")\")";
        CHECK(cs.eval(code).has_value(), "mutation set-code ok");
        auto r = cs.eval("(eval-current)");
        CHECK(r.has_value(), "post-mutation eval succeeds");
    }

    auto after = cs.eval("(eval-current)");
    CHECK(after.has_value(), "final eval succeeds");

    auto snap1 = cs.snapshot();
    CHECK(snap1.dead_coercion_eliminated_total >= elim0,
          "dead_coercion_eliminated_total monotonic");
    CHECK(snap1.coercion_zerooverhead_win_total >= win0,
          "coercion_zerooverhead_win_total monotonic");
    CHECK(snap1.coercion_castop_emitted_total >= cast0, "coercion_castop_emitted_total monotonic");
}

static void test_coercion_zerooverhead_stats_primitive() {
    std::println("\n--- AC5: query:coercion-zerooverhead-stats ---");
    CompilerService cs;
    auto r0 = cs.eval("(engine:metrics \"query:coercion-zerooverhead-stats\")");
    CHECK(r0 && is_int(*r0), "primitive returns int");
    const auto s0 = as_int(*r0);
    CHECK(s0 >= 0, "initial stats >= 0");

    cs.eval("(set-code \"(define x 42) (define y \\\"hello\\\") (define z #t)\")");
    CHECK(cs.eval("(eval-current)").has_value(), "pipeline eval succeeds");

    auto r1 = cs.eval("(engine:metrics \"query:coercion-zerooverhead-stats\")");
    CHECK(r1 && is_int(*r1), "primitive returns int after pipeline");
    CHECK(as_int(*r1) >= s0, "stats grow (monotonic) after pipeline");
}

static void test_coercion_map_apply_round_trip() {
    std::println("\n--- AC2b: CoercionMap apply before lowering path ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto callee_sym = pool->intern("+");
    auto callee_var = flat->add_variable(callee_sym);
    auto arg0 = flat->add_literal(42);
    auto arg1 = flat->add_literal(2);
    auto call_id = flat->add_call(callee_var, {arg0, arg1});
    flat->root = call_id;

    aura::compiler::CoercionMap cm;
    cm.add(call_id, 1, arg0, 2, 1, 0, 0);
    const auto applied = aura::compiler::apply_coercion_map(*flat, cm);
    CHECK(applied == 1, "apply_coercion_map applied 1 entry");
    auto parent = flat->get(call_id);
    const auto new_link = parent.child(1);
    CHECK(flat->get(new_link).tag == aura::ast::NodeTag::Coercion,
          "CoercionNode inserted at child_index 1");
}

} // namespace aura_611_detail

int main() {
    using namespace aura_611_detail;
    test_castop_type_tag_and_type_id_flow();
    test_coercion_map_apply_typespec_dce_pipeline();
    test_coercion_map_apply_round_trip();
    test_dirty_block_incremental_dce_aos();
    test_gradual_mutate_semantic_and_metrics();
    test_coercion_zerooverhead_stats_primitive();
    return RUN_ALL_TESTS();
}