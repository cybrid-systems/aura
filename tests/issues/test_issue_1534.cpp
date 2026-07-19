// @category: unit
// @reason: Issue #1534 — wire is_fn_epoch_stale into OpGuardShape dual-epoch fence
//
//   AC1: compile() captures fn epoch via capture_fn_epoch (fresh at capture epoch)
//   AC2: OpGuardShape path helper returns fresh (0) before epoch bump
//   AC3: after bump, is_fn_epoch_stale true + guard_shape_epoch_check returns 1
//   AC4: jit_epoch_stale_check_total grows on GuardShape epoch probe
//   AC5: dual-reader pairing — stale check + live_closure_stale_prevented /
//        jit_closure_stale_deopt counters advance together
//   AC6: never-captured / null name → check returns 0 (pass-through)
//   AC7: narrow_evidence fast-path still gated by epoch (stale forces deopt)
//   AC8: CompilerService atomic_bump + GuardShape check path end-to-end

#include "test_harness.hpp"
#include "compiler/aura_jit.h"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1534_detail {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::jit::AuraJIT;
using aura::jit::FlatBlock;
using aura::jit::FlatFunction;
using aura::jit::FlatInstruction;
using aura::test::g_failed;
using aura::test::g_passed;

// Minimal FlatFunction: ConstI64 → Return (same pattern as #1522).
// OpConstI64=1, OpReturn=20. GuardShape=52 for AC7.
static void fill_const_return(FlatFunction& fn, FlatInstruction* instrs, FlatBlock& block,
                              std::uint8_t* tags, const char* name) {
    instrs[0] = {};
    instrs[0].opcode = 1; // OpConstI64
    instrs[0].ops[0] = 0;
    instrs[0].ops[1] = 5;
    instrs[1] = {};
    instrs[1].opcode = 20; // OpReturn
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
    fn.arg_count = 0;
    fn.blocks = &block;
    fn.num_blocks = 1;
    fn.const_tags = tags;
    fn.func_id_map = nullptr;
    fn.num_callees = 0;
    fn.shape_map = nullptr;
    fn.escape_map = nullptr;
    fn.region = 0;
}

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static void ac1_compile_captures_epoch() {
    std::println("\n--- AC1: compile() captures fn epoch ---");
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    const auto e0 = aura_aot_func_table_epoch();
    FlatInstruction instrs[2];
    FlatBlock block;
    std::uint8_t tags[1];
    FlatFunction fn;
    fill_const_return(fn, instrs, block, tags, "fn1534_ac1");
    const auto c0 = jit.test_jit_epoch_stale_check_total();
    (void)jit.compile(fn);
    const auto c1 = jit.test_jit_epoch_stale_check_total();
    CHECK(c1 == c0 + 1, "compile capture_fn_epoch bumps jit_epoch_stale_check_total by 1");
    CHECK(!jit.is_fn_epoch_stale("fn1534_ac1", e0),
          "captured at table epoch → not stale at same epoch");
    // After table bump, stale.
    aura_aot_bump_func_table_epoch();
    const auto e1 = aura_aot_func_table_epoch();
    CHECK(e1 > e0, "table epoch advanced");
    CHECK(jit.is_fn_epoch_stale("fn1534_ac1", e1), "post-bump → is_fn_epoch_stale true");
    aura_set_jit_batch_deopt_target(nullptr);
}

static void ac2_guard_check_fresh() {
    std::println("\n--- AC2: GuardShape epoch check fresh before bump ---");
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    FlatInstruction instrs[2];
    FlatBlock block;
    std::uint8_t tags[1];
    FlatFunction fn;
    fill_const_return(fn, instrs, block, tags, "fn1534_ac2");
    (void)jit.compile(fn);
    // Same epoch as capture → check returns 0 (fresh).
    const int r = aura_jit_guard_shape_epoch_check("fn1534_ac2");
    CHECK(r == 0, "guard_shape_epoch_check fresh → 0");
    aura_set_jit_batch_deopt_target(nullptr);
}

static void ac3_guard_check_stale_after_bump() {
    std::println("\n--- AC3: after bump, GuardShape check deopts (returns 1) ---");
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    FlatInstruction instrs[2];
    FlatBlock block;
    std::uint8_t tags[1];
    FlatFunction fn;
    fill_const_return(fn, instrs, block, tags, "fn1534_ac3");
    (void)jit.compile(fn);
    aura_aot_bump_func_table_epoch();
    CHECK(jit.is_fn_epoch_stale("fn1534_ac3", aura_aot_func_table_epoch()),
          "is_fn_epoch_stale true after bump");
    const int r = aura_jit_guard_shape_epoch_check("fn1534_ac3");
    CHECK(r == 1, "guard_shape_epoch_check stale → 1 (deopt path)");
    aura_set_jit_batch_deopt_target(nullptr);
}

static void ac4_counter_grows_on_probe() {
    std::println("\n--- AC4: jit_epoch_stale_check_total grows on GuardShape probe ---");
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    FlatInstruction instrs[2];
    FlatBlock block;
    std::uint8_t tags[1];
    FlatFunction fn;
    fill_const_return(fn, instrs, block, tags, "fn1534_ac4");
    (void)jit.compile(fn);
    const auto c0 = jit.test_jit_epoch_stale_check_total();
    (void)aura_jit_guard_shape_epoch_check("fn1534_ac4");
    (void)aura_jit_guard_shape_epoch_check("fn1534_ac4");
    const auto c1 = jit.test_jit_epoch_stale_check_total();
    CHECK(c1 == c0 + 2, "2 GuardShape epoch probes bump check_total by 2");
    // Pure is_fn_epoch_stale still does not bump (preserves #1477 AC7).
    const auto c2 = jit.test_jit_epoch_stale_check_total();
    (void)jit.is_fn_epoch_stale("fn1534_ac4", aura_aot_func_table_epoch());
    CHECK(jit.test_jit_epoch_stale_check_total() == c2,
          "is_fn_epoch_stale remains pure read (no bump)");
    aura_set_jit_batch_deopt_target(nullptr);
}

static void ac5_dual_reader_pairing() {
    std::println("\n--- AC5: dual-reader pairing (stale deopt + live_closure) ---");
    CompilerService cs;
    // Service ctor already wires aura_set_aot_metrics(&metrics_).
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "CompilerMetrics available");
    CHECK(aura_get_aot_metrics() != nullptr, "aot_metrics wired by service");
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    FlatInstruction instrs[2];
    FlatBlock block;
    std::uint8_t tags[1];
    FlatFunction fn;
    fill_const_return(fn, instrs, block, tags, "fn1534_ac5");
    (void)jit.compile(fn);
    const auto dual0 = aura_jit_closure_dual_check_total();
    const auto stale0 = aura_jit_closure_stale_deopt_total();
    const auto live0 = m->compiler_live_closure_stale_prevented_total.load();
    const auto cm_chk0 = m->jit_epoch_stale_check_total.load();
    aura_aot_bump_func_table_epoch();
    const int r = aura_jit_guard_shape_epoch_check("fn1534_ac5");
    CHECK(r == 1, "stale → deopt");
    CHECK(aura_jit_closure_dual_check_total() > dual0, "dual_check_total advanced");
    CHECK(aura_jit_closure_stale_deopt_total() > stale0, "stale_deopt_total advanced");
    CHECK(m->compiler_live_closure_stale_prevented_total.load() > live0,
          "compiler_live_closure_stale_prevented_total advanced");
    CHECK(m->jit_epoch_stale_check_total.load() > cm_chk0,
          "CompilerMetrics jit_epoch_stale_check_total advanced");
    aura_set_jit_batch_deopt_target(nullptr);
}

static void ac6_pass_through() {
    std::println("\n--- AC6: never-captured / null → pass-through ---");
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    CHECK(aura_jit_guard_shape_epoch_check("never_compiled_1534") == 0, "never-captured → 0");
    CHECK(aura_jit_guard_shape_epoch_check(nullptr) == 0, "nullptr → 0");
    CHECK(aura_jit_guard_shape_epoch_check("") == 0, "empty → 0");
    aura_set_jit_batch_deopt_target(nullptr);
}

static void ac7_narrow_evidence_gated_by_epoch() {
    std::println("\n--- AC7: narrow_evidence still gated by epoch fence ---");
    // Lowering emits epoch check before narrow_evidence trust. Verify the
    // C helper (the runtime callee of that IR) returns stale after bump even
    // when we would have trusted narrow_evidence at the IR level.
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    FlatInstruction instrs[3];
    instrs[0] = {};
    instrs[0].opcode = 1; // OpConstI64
    instrs[0].ops[0] = 0;
    instrs[0].ops[1] = 42;
    instrs[1] = {};
    instrs[1].opcode = 52;         // OpGuardShape
    instrs[1].ops[0] = 1;          // result slot
    instrs[1].ops[1] = 0;          // arg slot
    instrs[1].ops[2] = 1;          // expected SHAPE_INT
    instrs[1].narrow_evidence = 1; // would skip shape check if epoch fresh
    instrs[2] = {};
    instrs[2].opcode = 20; // OpReturn
    instrs[2].ops[0] = 1;
    FlatBlock block{};
    block.id = 0;
    block.instructions = instrs;
    block.num_instructions = 3;
    std::uint8_t tags[2] = {0, 0};
    FlatFunction fn{};
    fn.name = "fn1534_ac7";
    fn.entry_block = 0;
    fn.local_count = 2;
    fn.blocks = &block;
    fn.num_blocks = 1;
    fn.const_tags = tags;
    (void)jit.compile(fn);
    // Fresh: check 0 (narrow_evidence path allowed).
    CHECK(aura_jit_guard_shape_epoch_check("fn1534_ac7") == 0,
          "fresh + narrow_evidence → epoch check 0 (allow fast-path)");
    aura_aot_bump_func_table_epoch();
    // Stale: check 1 even though narrow_evidence is set on the IR.
    CHECK(aura_jit_guard_shape_epoch_check("fn1534_ac7") == 1,
          "stale → epoch check 1 (force deopt despite narrow_evidence)");
    aura_set_jit_batch_deopt_target(nullptr);
}

static void ac8_service_atomic_bump_e2e() {
    std::println("\n--- AC8: service atomic_bump + GuardShape check e2e ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");
    // Compile via local AuraJIT for capture_fn_epoch; aot_metrics already
    // wired by service ctor so live_closure_stale_prevented is observable.
    AuraJIT jit;
    aura_set_jit_batch_deopt_target(&jit);
    FlatInstruction instrs[2];
    FlatBlock block;
    std::uint8_t tags[1];
    FlatFunction fn;
    fill_const_return(fn, instrs, block, tags, "fn1534_ac8");
    (void)jit.compile(fn);
    const auto e0 = cs.bridge_epoch();
    const auto table0 = aura_aot_func_table_epoch();
    cs.public_atomic_bump_epochs_and_stamp_bridge("fn1534_ac8");
    CHECK(cs.bridge_epoch() > e0, "bridge_epoch bumped");
    CHECK(aura_aot_func_table_epoch() > table0, "AOT table epoch bumped in lockstep");
    CHECK(jit.is_fn_epoch_stale("fn1534_ac8", aura_aot_func_table_epoch()),
          "fn stale vs post-bump table epoch");
    const auto live0 = m->compiler_live_closure_stale_prevented_total.load();
    const int r = aura_jit_guard_shape_epoch_check("fn1534_ac8");
    CHECK(r == 1, "GuardShape epoch check deopts after service atomic_bump");
    CHECK(m->compiler_live_closure_stale_prevented_total.load() > live0,
          "live_closure_stale_prevented pairs with GuardShape stale deopt");
    aura_set_jit_batch_deopt_target(nullptr);
}

} // namespace aura_issue_1534_detail

int main() {
    using namespace aura_issue_1534_detail;
    std::println("=== Issue #1534: GuardShape dual-epoch fence wire ===");
    ac1_compile_captures_epoch();
    ac2_guard_check_fresh();
    ac3_guard_check_stale_after_bump();
    ac4_counter_grows_on_probe();
    ac5_dual_reader_pairing();
    ac6_pass_through();
    ac7_narrow_evidence_gated_by_epoch();
    ac8_service_atomic_bump_e2e();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
