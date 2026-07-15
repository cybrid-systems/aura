// @category: unit
// @reason: pure C++ IR + DeadCoercionEliminationPass; no CompilerService
//
// test_issue_1418.cpp — Issue #1418: DeadCoercionEliminationPass
// wire-up to pass_manager pipeline.
//
// Background: DeadCoercionEliminationPass lived in pass_manager.ixx
// and ran on the default eval() path + post-mutate re-lower
// (run_coercion_elim_on_function), but three run_pipeline packs
// (eval_ir / exec_jit / hot-swap) omitted it. Dead CastOps then
// accumulated across evolve! / IR-direct / JIT paths.
//
// Production fix (service.ixx): those three packs now include DCE
// after ConstantFolding and call accumulate_coercion_pass_metrics.
//
// This file asserts the pass contract from the issue ACs:
//   AC1: synthetic IR with 5 CastOps (2 identity + 3 necessary)
//        → 2 eliminated, 3 CastOps remain
//   AC2: run_pipeline(mod, dce) invokes the pass (Pass concept)
//   AC3: eliminated_count increments (maps to
//        compile:dead-coercion-eliminated / metrics)
//   AC4: keep_for_debug leaves all 5 CastOps intact
//   AC5: residual CastOps are the non-identity ones (type_id mismatch)

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.compiler.pass_manager;
import aura.compiler.ir;

namespace test_issue_1418_detail {

using aura::compiler::DeadCoercionEliminationPass;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;

// Build IR with 5 CastOps:
//   - 2 identity: ConstI64 type_id=1 → CastOp type_id=1 (Rule 1 elide)
//   - 3 necessary: ConstI64 type_id=0 → CastOp type_id=1, type_tag=Int
//     (no ground source type → Rule 1 fails; type_tag=0 so Rule 3 N/A)
//
// Layout per pair: [ConstI64 slot S, CastOp result S+1 from S]
static IRModule build_5_castops_two_redundant() {
    IRModule mod;
    IRFunction func;
    func.id = 0;
    func.name = "dce1418";
    func.entry_block = 0;
    func.local_count = 16;
    func.arg_count = 0;

    aura::ir::BasicBlock blk;
    blk.id = 0;
    auto& ins = blk.instructions;

    // Identity #1: ConstI64 slot0 type_id=1, CastOp slot1 ← 0 type_id=1
    ins.push_back(IRInstruction{IROpcode::ConstI64, {0, 7, 0, 0}, 0, 1});
    ins.push_back(IRInstruction{IROpcode::CastOp, {1, 0, 0, 0}, 0, 1});

    // Identity #2: ConstI64 slot2 type_id=1, CastOp slot3 ← 2 type_id=1
    ins.push_back(IRInstruction{IROpcode::ConstI64, {2, 9, 0, 0}, 0, 1});
    ins.push_back(IRInstruction{IROpcode::CastOp, {3, 2, 0, 0}, 0, 1});

    // Necessary #1–3: Const type_id=0, CastOp wants Int (type_id=1, tag=0)
    ins.push_back(IRInstruction{IROpcode::ConstI64, {4, 1, 0, 0}, 0, 0});
    ins.push_back(IRInstruction{IROpcode::CastOp, {5, 4, 0, 0}, 0, 1});

    ins.push_back(IRInstruction{IROpcode::ConstI64, {6, 2, 0, 0}, 0, 0});
    ins.push_back(IRInstruction{IROpcode::CastOp, {7, 6, 0, 0}, 0, 1});

    ins.push_back(IRInstruction{IROpcode::ConstI64, {8, 3, 0, 0}, 0, 0});
    ins.push_back(IRInstruction{IROpcode::CastOp, {9, 8, 0, 0}, 0, 1});

    // Return one of the necessary cast results so the block is well-formed.
    ins.push_back(IRInstruction{IROpcode::Return, {5, 0, 0, 0}, 0, 0});

    func.blocks.push_back(std::move(blk));
    mod.functions.push_back(std::move(func));
    return mod;
}

static int count_castops(const IRModule& mod) {
    int n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::CastOp)
                    ++n;
    return n;
}

// ── AC1: 2 removed, 3 kept ──────────────────────────────────────

bool test_ac1_two_removed_three_kept() {
    std::println("\n--- AC1: 5 CastOps → 2 eliminated, 3 kept ---");
    auto mod = build_5_castops_two_redundant();
    CHECK(count_castops(mod) == 5, "AC1.setup: synthetic IR has 5 CastOps");

    DeadCoercionEliminationPass dce;
    dce.run(mod);

    const auto elim = dce.eliminated_count();
    const auto remaining = count_castops(mod);
    std::println("  eliminated={} remaining_castops={}", elim, remaining);
    CHECK(elim == 2, "AC1: exactly 2 redundant CastOps eliminated");
    CHECK(remaining == 3, "AC1: exactly 3 necessary CastOps remain");
    return true;
}

// ── AC2: run_pipeline invokes DCE (Pass concept) ────────────────

bool test_ac2_run_pipeline_invokes_dce() {
    std::println("\n--- AC2: run_pipeline(mod, dce) invokes pass ---");
    auto mod = build_5_castops_two_redundant();
    DeadCoercionEliminationPass dce;
    const bool ok = aura::compiler::run_pipeline(mod, dce);
    std::println("  run_pipeline ok={} eliminated={}", ok, dce.eliminated_count());
    CHECK(ok, "AC2: run_pipeline returns true (no error)");
    CHECK(dce.eliminated_count() == 2, "AC2: DCE inside run_pipeline eliminated 2");
    CHECK(count_castops(mod) == 3, "AC2: residual CastOps = 3 after pipeline");
    return true;
}

// ── AC3: eliminated_count increments (metric source) ────────────

bool test_ac3_eliminated_count_increments() {
    std::println("\n--- AC3: eliminated_count is the metric source ---");
    auto mod = build_5_castops_two_redundant();
    DeadCoercionEliminationPass dce;
    CHECK(dce.eliminated_count() == 0, "AC3.setup: count starts at 0");
    dce.run(mod);
    // service.ixx accumulate_coercion_pass_metrics does:
    //   metrics_.dead_coercion_eliminated_total.fetch_add(dce.eliminated_count())
    // so a non-zero eliminated_count is what feeds
    // (compile:dead-coercion-stats) / snapshot().dead_coercion_eliminated_total.
    CHECK(dce.eliminated_count() > 0, "AC3: eliminated_count increments after run");
    CHECK(dce.eliminated_count() == 2,
          "AC3: eliminated_count == 2 (compile:dead-coercion-eliminated)");
    CHECK(dce.elapsed_us() > 0 || dce.eliminated_count() > 0,
          "AC3: pass did observable work (elapsed or elim)");
    return true;
}

// ── AC4: keep_for_debug leaves all CastOps ──────────────────────

bool test_ac4_keep_for_debug() {
    std::println("\n--- AC4: keep_for_debug leaves all 5 CastOps ---");
    auto mod = build_5_castops_two_redundant();
    DeadCoercionEliminationPass dce;
    dce.set_keep_for_debug(true);
    dce.run(mod);
    std::println("  eliminated={} kept_for_debug={} remaining={}", dce.eliminated_count(),
                 dce.kept_for_debug_count(), count_castops(mod));
    CHECK(dce.eliminated_count() == 0, "AC4: keep_for_debug eliminates 0");
    CHECK(dce.kept_for_debug_count() == 5, "AC4: kept_for_debug counts all 5 CastOps");
    CHECK(count_castops(mod) == 5, "AC4: IR still has 5 CastOps");
    return true;
}

// ── AC5: residual CastOps are the non-identity ones ─────────────

bool test_ac5_residual_are_necessary() {
    std::println("\n--- AC5: residual CastOps are non-identity ---");
    auto mod = build_5_castops_two_redundant();
    DeadCoercionEliminationPass dce;
    dce.run(mod);

    int residual = 0;
    for (const auto& f : mod.functions) {
        for (const auto& b : f.blocks) {
            for (const auto& i : b.instructions) {
                if (i.opcode != IROpcode::CastOp)
                    continue;
                ++residual;
                // Necessary casts have source Const with type_id=0;
                // CastOp itself has type_id=1. Identity ones became Local.
                CHECK(i.type_id == 1, "AC5: residual CastOp still targets type_id=1");
            }
        }
    }
    // Identity casts (slots 1 and 3) must now be Local.
    const auto& ins = mod.functions[0].blocks[0].instructions;
    CHECK(ins[1].opcode == IROpcode::Local, "AC5: identity CastOp #1 → Local");
    CHECK(ins[3].opcode == IROpcode::Local, "AC5: identity CastOp #2 → Local");
    CHECK(residual == 3, "AC5: three necessary CastOps remain");
    return true;
}

} // namespace test_issue_1418_detail

int aura_issue_1418_run() {
    using namespace test_issue_1418_detail;
    std::println("=== Issue #1418: DeadCoercionEliminationPass pipeline wire-up ===");
    bool all_ok = true;
    all_ok &= test_ac1_two_removed_three_kept();
    all_ok &= test_ac2_run_pipeline_invokes_dce();
    all_ok &= test_ac3_eliminated_count_increments();
    all_ok &= test_ac4_keep_for_debug();
    all_ok &= test_ac5_residual_are_necessary();
    if (all_ok && g_failed == 0) {
        std::println("\n=== ALL ACs PASS ===");
        return 0;
    }
    std::println("\n=== Some ACs FAILED (g_failed={}) ===", g_failed);
    return 1;
}

int main() {
    return aura_issue_1418_run();
}
