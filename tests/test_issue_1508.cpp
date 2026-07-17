// @category: integration
// @reason: Issue #1508 — JIT aura_closure_call dual check
// (bridge_epoch + defuse/env_frame) + interpreter-deopt fallback.
//
//   AC1: aura_is_jit_closure_fresh matches table + defuse epochs
//   AC2: aura_closure_call stamps provenance at alloc
//   AC3: table-epoch bump → stale deopt (refuse native call, no UAF)
//   AC4: defuse bump → stale deopt
//   AC5: dual_check / stale_deopt / safe_fallback counters grow
//   AC6: 1000-iter alloc/mutate/call stress, no crash
//   AC7: regression — free path still safe after dual check

#include "test_harness.hpp"
#include "compiler/runtime_shared.h"
#include "compiler/aura_jit_bridge.h"

#include <cstdint>
#include <cstdlib>
#include <print>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

extern "C" void aura_reset_runtime();
extern "C" int64_t aura_alloc_closure(int64_t func_id);
extern "C" int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc);
extern "C" void aura_free_closure(int64_t closure_id);
extern "C" int aura_closure_is_freed(int64_t closure_id);
extern "C" uint64_t aura_deopt_count();

namespace aura_issue_1508_detail {

using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;

static void ac1_helper_freshness() {
    std::println("\n--- AC1: aura_is_jit_closure_fresh helper ---");
    // Strict default before first freshness call (static legacy_trust init).
    setenv("AURA_BRIDGE_EPOCH_LEGACY_TRUST", "0", 1);
    // Align host epochs to known values.
    aura_set_aot_defuse_version(10);
    // Bump table once so we know current ≠ 0.
    const auto e0 = aura_aot_func_table_epoch();
    aura_aot_bump_func_table_epoch();
    const auto e1 = aura_aot_func_table_epoch();
    CHECK(e1 > e0, "table epoch advances on bump");

    CHECK(aura_is_jit_closure_fresh(e1, 10), "matching bridge+defuse is fresh");
    CHECK(!aura_is_jit_closure_fresh(e1 - 1, 10), "stale bridge is not fresh");
    CHECK(!aura_is_jit_closure_fresh(e1, 11), "stale defuse is not fresh");
    // Issue #1491: unstamped capture while tracking is active is STALE
    // (aligns with is_bridge_stale / is_env_frame_stale).
    CHECK(!aura_is_jit_closure_fresh(0, 10), "bridge=0 + active table is stale (strict #1491)");
    CHECK(!aura_is_jit_closure_fresh(e1, 0), "defuse=0 + active defuse is stale (strict #1491)");
    CHECK(!aura_is_jit_closure_fresh(0, 0), "both zero under active tracking is stale");
    // Defuse domain inactive → zero defuse capture is ok with matching bridge.
    aura_set_aot_defuse_version(0);
    CHECK(aura_is_jit_closure_fresh(e1, 0), "matching bridge + defuse tracking off is fresh");
}

static void ac2_stamp_on_alloc() {
    std::println("\n--- AC2: alloc stamps provenance; fresh call passes dual check ---");
    aura_set_aot_defuse_version(42);
    aura_aot_bump_func_table_epoch(); // move past any prior
    const auto dual0 = aura_jit_closure_dual_check_total();
    auto id = aura_alloc_closure(/*func_id=*/7);
    CHECK(id >= 0, "alloc_closure ok");
    // No matching JIT fn for func_id 7 → call returns 0 after dual check
    // (fresh), without taking the stale-deopt path.
    const auto deopt0 = aura_jit_closure_stale_deopt_total();
    auto r = aura_closure_call(id, nullptr, 0);
    (void)r;
    CHECK(aura_jit_closure_dual_check_total() > dual0, "dual_check bumped on call");
    CHECK(aura_jit_closure_stale_deopt_total() == deopt0, "fresh call does not bump stale_deopt");
    aura_free_closure(id);
}

static void ac3_table_epoch_stale_deopt() {
    std::println("\n--- AC3: table epoch bump → stale deopt ---");
    aura_set_aot_defuse_version(100);
    auto id = aura_alloc_closure(3);
    CHECK(id >= 0, "alloc for table-stale path");

    const auto deopt0 = aura_jit_closure_stale_deopt_total();
    const auto safe0 = aura_jit_closure_safe_fallbacks();
    const auto gdeopt0 = aura_deopt_count();

    aura_aot_bump_func_table_epoch(); // invalidate bridge stamp
    auto r = aura_closure_call(id, nullptr, 0);
    CHECK(r == 0, "stale call returns 0 (safe refuse, no UAF)");
    CHECK(aura_jit_closure_stale_deopt_total() > deopt0, "stale_deopt bumped");
    CHECK(aura_jit_closure_safe_fallbacks() > safe0, "safe_fallback bumped");
    CHECK(aura_deopt_count() > gdeopt0, "aura_deopt_inc fired");
    aura_free_closure(id);
}

static void ac4_defuse_stale_deopt() {
    std::println("\n--- AC4: defuse bump → stale deopt ---");
    aura_set_aot_defuse_version(200);
    auto id = aura_alloc_closure(4);
    CHECK(id >= 0, "alloc for defuse-stale path");

    const auto deopt0 = aura_jit_closure_stale_deopt_total();
    aura_set_aot_defuse_version(201); // mutate epoch advance
    auto r = aura_closure_call(id, nullptr, 0);
    CHECK(r == 0, "defuse-stale call returns 0");
    CHECK(aura_jit_closure_stale_deopt_total() > deopt0, "stale_deopt after defuse bump");
    aura_free_closure(id);
}

static void ac5_metrics_via_service() {
    std::println("\n--- AC5: CompilerService wires aot metrics ---");
    CompilerService cs; // ctor sets aura_set_aot_metrics(&metrics_)
    (void)cs;
    aura_set_aot_defuse_version(cs.evaluator().defuse_version() + 1);
    auto id = aura_alloc_closure(5);
    const auto dual0 = aura_jit_closure_dual_check_total();
    aura_aot_bump_func_table_epoch();
    (void)aura_closure_call(id, nullptr, 0);
    CHECK(aura_jit_closure_dual_check_total() > dual0,
          "dual_check visible via service-wired metrics");
    CHECK(aura_jit_closure_stale_deopt_total() > 0, "stale_deopt non-zero after forced stale");
    aura_free_closure(id);
}

static void ac6_stress() {
    std::println("\n--- AC6: 1000-iter alloc / epoch-bump / call stress ---");
    int crashes = 0;
    for (int i = 0; i < 1000; ++i) {
        aura_set_aot_defuse_version(static_cast<std::uint64_t>(1000 + i));
        auto id = aura_alloc_closure(i % 17);
        if ((i % 3) == 0)
            aura_aot_bump_func_table_epoch();
        if ((i % 5) == 0)
            aura_set_aot_defuse_version(static_cast<std::uint64_t>(2000 + i));
        int64_t args[2] = {1, 2};
        (void)aura_closure_call(id, args, 2);
        aura_free_closure(id);
    }
    CHECK(crashes == 0, "1000-iter stress completed without crash");
    CHECK(aura_jit_closure_dual_check_total() >= 1000, "dual_check >= 1000 after stress");
}

static void ac7_free_regression() {
    std::println("\n--- AC7: free path still safe under dual check ---");
    auto id = aura_alloc_closure(9);
    aura_free_closure(id);
    CHECK(aura_closure_is_freed(id) == 1, "freed flag set");
    CHECK(aura_closure_call(id, nullptr, 0) == 0, "call freed → 0");
}

} // namespace aura_issue_1508_detail

int aura_issue_1508_run() {
    using namespace aura_issue_1508_detail;
    std::println("=== Issue #1508: JIT closure dual check + deopt ===");
    aura_reset_runtime();

    // Wire metrics so C counter readers are non-zero.
    CompilerService cs;
    (void)cs;

    ac1_helper_freshness();
    ac2_stamp_on_alloc();
    ac3_table_epoch_stale_deopt();
    ac4_defuse_stale_deopt();
    ac5_metrics_via_service();
    ac6_stress();
    ac7_free_regression();

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_1508_run();
}
#endif
