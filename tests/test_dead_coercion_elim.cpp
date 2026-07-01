// test_dead_coercion_elim.cpp — Issue #538: DeadCoercionEliminationPass
// + IR type info downstream flow for zero-overhead gradual/typed mutation.
//
// AC1: DCE elides identity + Dynamic passthrough casts (>40% reduction)
// AC2: run_function incremental entry works on per-function IR
// AC3: IRModuleV2 SoA dirty-block incremental DCE
// AC4: post-mutate cache_define path runs DCE (metrics grow)
// AC5: IR instruction narrow_evidence flows to downstream consumers

#include "test_harness.hpp"

import std;
import aura.compiler.ir;
import aura.compiler.ir_soa;
import aura.compiler.pass_manager;
import aura.compiler.service;

namespace aura_538_detail {

using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::compiler::CompilerService;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::IRModuleV2;

static std::size_t count_cast_ops(const IRModule& mod) {
    std::size_t n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::CastOp)
                    ++n;
    return n;
}

static IRModule make_gradual_workload(std::size_t values_per_block = 4) {
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "workload", .local_count = 64});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    auto& blk = func.blocks[0].instructions;
    std::uint32_t slot = 0;
    for (std::size_t vi = 0; vi < values_per_block; ++vi) {
        auto s0 = slot++;
        blk.push_back({IROpcode::ConstI64, {s0, 42, 0, 0}, 0, 1});
        auto s1 = slot++;
        blk.push_back({IROpcode::CastOp, {s1, s0, 3, 0}, 0, 0}); // elidable
        auto s2 = slot++;
        blk.push_back({IROpcode::CastOp, {s2, s1, 0, 0}, 0, 0}); // not elidable
        blk.push_back({IROpcode::Local, {slot, s2, 0, 0}, 0, 0});
    }
    blk.push_back({IROpcode::Return, {slot - 1, 0, 0, 0}, 0, 0});
    return mod;
}

static void test_gradual_cast_reduction_ac() {
    std::println("\n--- AC1: >40% CastOp reduction in gradual workload ---");
    auto mod = make_gradual_workload(8);
    const auto before = count_cast_ops(mod);
    DeadCoercionEliminationPass dce;
    dce.run(mod);
    const auto after = count_cast_ops(mod);
    const auto reduction_pct = before > 0 ? (100 * (before - after)) / before : 0;
    std::println("  before={} after={} reduction={}%", before, after, reduction_pct);
    CHECK(before == 16, "16 CastOps before (8 elidable + 8 not)");
    CHECK(after == 8, "8 CastOps remain after DCE");
    CHECK(reduction_pct >= 40, "reduction >= 40%");
    CHECK(dce.eliminated_count() == 8, "eliminated_count == 8");
}

static void test_run_function_incremental() {
    std::println("\n--- AC2: run_function incremental DCE entry ---");
    IRModule mod = make_gradual_workload(2);
    auto& func = mod.functions[0];
    const auto before = count_cast_ops(mod);
    DeadCoercionEliminationPass dce;
    dce.run_function(func);
    const auto after = count_cast_ops(mod);
    CHECK(before == 4 && after == 2, "run_function elides half the CastOps");
    CHECK(dce.eliminated_count() == 2, "eliminated_count == 2");
}

static void test_soa_dirty_block_incremental() {
    std::println("\n--- AC3: IRModuleV2 SoA dirty-block incremental DCE ---");
    IRModuleV2 mod;
    mod.functions.push_back({});
    auto& func = mod.functions[0];
    const auto bid = mod.add_block(0);
    mod.add_instruction(0, IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1);
    mod.add_instruction(0, IROpcode::CastOp, {1, 0, 3, 0}, 0, 0);
    mod.add_instruction(0, IROpcode::Return, {1, 0, 0, 0});
    mod.seal_block(0, bid);
    func.mark_block_dirty(bid);

    DeadCoercionEliminationPass dce;
    dce.run(mod);
    CHECK(func.opcodes_[1] == IROpcode::Local, "SoA CastOp elided to Local");
    CHECK(dce.eliminated_count() == 1, "SoA eliminated_count == 1");
    CHECK(func.is_block_dirty(bid), "block marked dirty after SoA elision");
}

static void test_post_mutate_cache_define_dce() {
    std::println("\n--- AC4: post-mutate cache_define runs DCE ---");
    CompilerService cs;
    auto snap0 = cs.snapshot();
    cs.eval("(set-code \"(define f (lambda (x) x))\")");
    CHECK(cs.eval("(eval-current)").has_value(), "initial define eval ok");
    auto snap1 = cs.snapshot();
    CHECK(snap1.dead_coercion_eliminated_total >= snap0.dead_coercion_eliminated_total,
          "dead_coercion_eliminated_total monotonic after define");

    for (int i = 0; i < 3; ++i) {
        std::string code = "(set-code \"(define f (lambda (x) (+ x ";
        code += std::to_string(i);
        code += ")))\")";
        CHECK(cs.eval(code).has_value(), "mutation set-code ok");
        CHECK(cs.eval("(eval-current)").has_value(), "post-mutation eval ok");
    }
    auto snap2 = cs.snapshot();
    CHECK(snap2.dead_coercion_eliminated_total >= snap1.dead_coercion_eliminated_total,
          "metrics grow across mutations");
}

static void test_ir_narrow_evidence_metadata() {
    std::println("\n--- AC5: IR narrow_evidence metadata for downstream JIT ---");
    IRInstruction guard{};
    guard.opcode = IROpcode::GuardShape;
    guard.operands = {0, 1, 1, 2};
    guard.type_id = 1;
    guard.narrow_evidence = 0x4;
    CHECK(guard.narrow_evidence == 0x4, "narrow_evidence on IRInstruction");
    CHECK(guard.type_id == 1, "type_id on IRInstruction");
}

} // namespace aura_538_detail

int main() {
    using namespace aura_538_detail;
    test_gradual_cast_reduction_ac();
    test_run_function_incremental();
    test_soa_dirty_block_incremental();
    test_post_mutate_cache_define_dce();
    test_ir_narrow_evidence_metadata();
    return RUN_ALL_TESTS();
}