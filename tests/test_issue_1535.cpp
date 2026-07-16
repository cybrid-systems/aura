// @category: unit
// @reason: Issue #1535 — Linear* dual-epoch fence (is_fn_epoch_stale + is_env_frame_stale)
//
//   AC1: Linear epoch safety check fresh after compile (no mutate)
//   AC2: mid-Linear mutate (table/defuse bump) → check returns 1 (runtime error/deopt)
//   AC3: safe post-Linear path (no mid-op bump) → check returns 0
//   AC4: is_env_frame_stale half via linear env context (stale frame vs defuse)
//   AC5: jit_epoch_stale_check_total + compiler_live_closure_stale_prevented pair
//   AC6: Move/Borrow/Drop opcodes all route through the dual-check helper
//   AC7: null/empty fn + no env context → pass-through (0)
//   AC8: service atomic_bump mid-Linear → stale + live_closure prevented

#include "test_harness.hpp"
#include "compiler/aura_jit.h"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1535_detail {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::jit::AuraJIT;
using aura::jit::FlatBlock;
using aura::jit::FlatFunction;
using aura::jit::FlatInstruction;
using aura::test::g_failed;
using aura::test::g_passed;

// Op codes from aura_jit.cpp / ir.ixx
static constexpr std::uint32_t kOpConstI64 = 1;
static constexpr std::uint32_t kOpReturn = 20;
static constexpr std::uint32_t kOpMoveOp = 45;
static constexpr std::uint32_t kOpBorrowOp = 46;
static constexpr std::uint32_t kOpDropOp = 48;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static void fill_const_return(FlatFunction& fn, FlatInstruction* instrs, FlatBlock& block,
                              std::uint8_t* tags, const char* name) {
    instrs[0] = {};
    instrs[0].opcode = kOpConstI64;
    instrs[0].ops[0] = 0;
    instrs[0].ops[1] = 7;
    instrs[1] = {};
    instrs[1].opcode = kOpReturn;
    instrs[1].ops[0] = 0;
    block = {};
    block.id = 0;
    block.instructions = instrs;
    block.num_instructions = 2;
    tags[0] = 0;
    fn = {};
    fn.name = name;
    fn.entry_block = 0;
    fn.local_count = 1;
    fn.blocks = &block;
    fn.num_blocks = 1;
    fn.const_tags = tags;
}

static void ac1_fresh_after_compile() {
    std::println("\n--- AC1: Linear epoch check fresh after compile ---");
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    aura_jit_clear_linear_env_context();
    FlatInstruction instrs[2];
    FlatBlock block;
    std::uint8_t tags[1];
    FlatFunction fn;
    fill_const_return(fn, instrs, block, tags, "fn1535_ac1");
    (void)jit.compile(fn);
    // No bump → both halves fresh.
    CHECK(aura_jit_linear_epoch_safety_check("fn1535_ac1", /*state=*/1, kOpMoveOp) == 0,
          "fresh after compile → Linear check 0 (safe)");
    aura_set_jit_batch_deopt_target(nullptr);
}

static void ac2_mid_linear_mutate_stale() {
    std::println("\n--- AC2: mid-Linear mutate → runtime error (stale=1) ---");
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    aura_jit_clear_linear_env_context();
    FlatInstruction instrs[2];
    FlatBlock block;
    std::uint8_t tags[1];
    FlatFunction fn;
    fill_const_return(fn, instrs, block, tags, "fn1535_ac2");
    (void)jit.compile(fn);
    // Simulate mutate mid-Linear-op: bump table epoch after capture.
    aura_aot_bump_func_table_epoch();
    CHECK(jit.is_fn_epoch_stale("fn1535_ac2", aura_aot_func_table_epoch()),
          "is_fn_epoch_stale true after mid-op bump");
    const int r = aura_jit_linear_epoch_safety_check("fn1535_ac2", /*state=*/1, kOpMoveOp);
    CHECK(r == 1, "mid-Linear mutate → Linear epoch check 1 (runtime deopt/error)");
    aura_set_jit_batch_deopt_target(nullptr);
}

static void ac3_safe_post_linear() {
    std::println("\n--- AC3: safe mutate post-Linear (no mid-op bump) → no error ---");
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    aura_jit_clear_linear_env_context();
    FlatInstruction instrs[2];
    FlatBlock block;
    std::uint8_t tags[1];
    FlatFunction fn;
    fill_const_return(fn, instrs, block, tags, "fn1535_ac3");
    (void)jit.compile(fn);
    // Linear check at same epoch as capture → safe.
    CHECK(aura_jit_linear_epoch_safety_check("fn1535_ac3", /*state=*/1, kOpDropOp) == 0,
          "same-epoch Linear check → 0 (safe, no error)");
    // Re-capture (as recompile would) then check again.
    jit.capture_fn_epoch("fn1535_ac3", aura_aot_func_table_epoch());
    CHECK(aura_jit_linear_epoch_safety_check("fn1535_ac3", /*state=*/1, kOpBorrowOp) == 0,
          "post-recapture Linear check → 0");
    aura_set_jit_batch_deopt_target(nullptr);
}

static void ac4_env_frame_stale_half() {
    std::println("\n--- AC4: is_env_frame_stale half via linear env context ---");
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    // Never-captured fn → is_fn_epoch_stale pass-through; env half decides.
    aura_set_aot_defuse_version(100);
    aura_jit_set_linear_env_context(/*env_id=*/42, /*frame_version=*/50); // 50 < 100 → stale
    CHECK(aura_jit_linear_epoch_safety_check("never_compiled_env", /*state=*/1, kOpMoveOp) == 1,
          "env frame stale (50 < 100) → Linear check 1");
    // Fresh frame.
    aura_jit_set_linear_env_context(/*env_id=*/42, /*frame_version=*/100);
    CHECK(aura_jit_linear_epoch_safety_check("never_compiled_env", /*state=*/1, kOpMoveOp) == 0,
          "env frame fresh (100 == 100) → Linear check 0");
    // frame > current → defensive fresh.
    aura_jit_set_linear_env_context(/*env_id=*/42, /*frame_version=*/150);
    CHECK(aura_jit_linear_epoch_safety_check("never_compiled_env", /*state=*/1, kOpBorrowOp) == 0,
          "env frame > current → Linear check 0");
    // Tracking inactive (defuse 0) → not stale.
    aura_set_aot_defuse_version(0);
    aura_jit_set_linear_env_context(/*env_id=*/42, /*frame_version=*/1);
    CHECK(aura_jit_linear_epoch_safety_check("never_compiled_env", /*state=*/1, kOpDropOp) == 0,
          "defuse tracking inactive → Linear check 0");
    aura_jit_clear_linear_env_context();
    aura_set_jit_batch_deopt_target(nullptr);
}

static void ac5_dual_metric_pairing() {
    std::println("\n--- AC5: jit_epoch_stale_check + live_closure_stale_prevented pair ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "CompilerMetrics");
    CHECK(aura_get_aot_metrics() != nullptr, "aot_metrics wired");
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    aura_jit_clear_linear_env_context();
    FlatInstruction instrs[2];
    FlatBlock block;
    std::uint8_t tags[1];
    FlatFunction fn;
    fill_const_return(fn, instrs, block, tags, "fn1535_ac5");
    (void)jit.compile(fn);
    const auto chk0 = jit.test_jit_epoch_stale_check_total();
    const auto cm_chk0 = m->jit_epoch_stale_check_total.load();
    const auto live0 = m->compiler_live_closure_stale_prevented_total.load();
    const auto enf0 = m->linear_post_mutate_enforcements_total.load();
    aura_aot_bump_func_table_epoch();
    const int r = aura_jit_linear_epoch_safety_check("fn1535_ac5", /*state=*/1, kOpMoveOp);
    CHECK(r == 1, "stale Linear → deopt");
    CHECK(jit.test_jit_epoch_stale_check_total() > chk0, "AuraJIT jit_epoch_stale_check_total +");
    CHECK(m->jit_epoch_stale_check_total.load() > cm_chk0,
          "CompilerMetrics jit_epoch_stale_check_total +");
    CHECK(m->compiler_live_closure_stale_prevented_total.load() > live0,
          "compiler_live_closure_stale_prevented_total +");
    CHECK(m->linear_post_mutate_enforcements_total.load() > enf0,
          "linear_post_mutate_enforcements_total + on stale Linear");
    aura_set_jit_batch_deopt_target(nullptr);
}

static void ac6_all_linear_opcodes() {
    std::println("\n--- AC6: Move/Borrow/Drop all route through dual-check ---");
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    aura_jit_clear_linear_env_context();
    FlatInstruction instrs[2];
    FlatBlock block;
    std::uint8_t tags[1];
    FlatFunction fn;
    fill_const_return(fn, instrs, block, tags, "fn1535_ac6");
    (void)jit.compile(fn);
    aura_aot_bump_func_table_epoch();
    CHECK(aura_jit_linear_epoch_safety_check("fn1535_ac6", 1, kOpMoveOp) == 1, "MoveOp stale");
    // Re-capture so Borrow sees fresh then re-bump for Drop.
    jit.capture_fn_epoch("fn1535_ac6", aura_aot_func_table_epoch());
    CHECK(aura_jit_linear_epoch_safety_check("fn1535_ac6", 1, kOpBorrowOp) == 0,
          "BorrowOp fresh after recapture");
    aura_aot_bump_func_table_epoch();
    CHECK(aura_jit_linear_epoch_safety_check("fn1535_ac6", 1, kOpBorrowOp) == 1, "BorrowOp stale");
    jit.capture_fn_epoch("fn1535_ac6", aura_aot_func_table_epoch());
    aura_aot_bump_func_table_epoch();
    CHECK(aura_jit_linear_epoch_safety_check("fn1535_ac6", 1, kOpDropOp) == 1, "DropOp stale");
    aura_set_jit_batch_deopt_target(nullptr);
}

static void ac7_pass_through() {
    std::println("\n--- AC7: null/empty + no env context → pass-through ---");
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    aura_jit_clear_linear_env_context();
    aura_set_aot_defuse_version(0);
    CHECK(aura_jit_linear_epoch_safety_check(nullptr, 1, kOpMoveOp) == 0, "nullptr fn → 0");
    CHECK(aura_jit_linear_epoch_safety_check("", 1, kOpDropOp) == 0, "empty fn → 0");
    CHECK(aura_jit_linear_epoch_safety_check("unknown_fn", 0, kOpBorrowOp) == 0,
          "never-captured + no env → 0");
    aura_set_jit_batch_deopt_target(nullptr);
}

static void ac8_service_atomic_bump_e2e() {
    std::println("\n--- AC8: service atomic_bump mid-Linear → stale + prevented ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    aura_jit_clear_linear_env_context();
    FlatInstruction instrs[2];
    FlatBlock block;
    std::uint8_t tags[1];
    FlatFunction fn;
    fill_const_return(fn, instrs, block, tags, "fn1535_ac8");
    (void)jit.compile(fn);
    // Stamp env context at "linear create" defuse.
    const auto defuse0 = aura_get_aot_defuse_version();
    aura_jit_set_linear_env_context(/*env_id=*/7, /*frame_version=*/defuse0 == 0 ? 1 : defuse0);
    if (defuse0 == 0)
        aura_set_aot_defuse_version(1);
    const auto live0 = m->compiler_live_closure_stale_prevented_total.load();
    cs.public_atomic_bump_epochs_and_stamp_bridge("fn1535_ac8");
    // Both halves likely stale after atomic bump.
    const int r = aura_jit_linear_epoch_safety_check("fn1535_ac8", /*state=*/1, kOpMoveOp);
    CHECK(r == 1, "after service atomic_bump → Linear check 1");
    CHECK(m->compiler_live_closure_stale_prevented_total.load() > live0,
          "live_closure_stale_prevented pairs with Linear stale path");
    aura_jit_clear_linear_env_context();
    aura_set_jit_batch_deopt_target(nullptr);
}

} // namespace aura_issue_1535_detail

int main() {
    using namespace aura_issue_1535_detail;
    std::println("=== Issue #1535: Linear post-invalidate dual-epoch fence ===");
    ac1_fresh_after_compile();
    ac2_mid_linear_mutate_stale();
    ac3_safe_post_linear();
    ac4_env_frame_stale_half();
    ac5_dual_metric_pairing();
    ac6_all_linear_opcodes();
    ac7_pass_through();
    ac8_service_atomic_bump_e2e();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
