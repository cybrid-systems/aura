// @category: unit
// @reason: pure AuraJIT dual-epoch fence; no CompilerService required
//
// Issue #1477 — JIT-side dual-epoch fence (capture_fn_epoch +
// is_fn_epoch_stale). Re-enabled by #1527 after aura_jit_test_objects
// gained prim_dispatch stub + contract_handler link deps.
//
//   AC1: never-captured name → is_fn_epoch_stale returns false (pass-through)
//   AC2: capture + same epoch → not stale
//   AC3: capture then different epoch → stale
//   AC4: re-capture advances; old epoch is stale, new is fresh
//   AC5: 1000-iter capture stress (final stale vs fresh)
//   AC6: capture_fn_epoch bumps jit_epoch_stale_check_total
//   AC7: is_fn_epoch_stale is a pure read (does not bump counter)
//   AC8: multi-fn independence (stale vs fresh across names)

#include "test_harness.hpp"
#include "compiler/aura_jit.h"

#include <cstdint>
#include <print>
#include <string>

import std;

namespace aura_issue_1477_detail {

using aura::jit::AuraJIT;
using aura::test::g_failed;
using aura::test::g_passed;

static void ac1_never_captured() {
    std::println("\n--- AC1: never-captured pass-through ---");
    AuraJIT jit;
    CHECK(!jit.is_fn_epoch_stale("never_compiled", 42),
          "never-captured → not stale (pass-through)");
    CHECK(!jit.is_fn_epoch_stale(nullptr, 1), "nullptr name → not stale");
}

static void ac2_capture_same_epoch() {
    std::println("\n--- AC2: capture + same epoch → fresh ---");
    AuraJIT jit;
    jit.capture_fn_epoch("f", 10);
    CHECK(!jit.is_fn_epoch_stale("f", 10), "capture 10 + current 10 → not stale");
}

static void ac3_capture_then_stale() {
    std::println("\n--- AC3: capture then different epoch → stale ---");
    AuraJIT jit;
    jit.capture_fn_epoch("g", 1);
    CHECK(jit.is_fn_epoch_stale("g", 2), "capture 1 + current 2 → stale");
    CHECK(!jit.is_fn_epoch_stale("g", 1), "capture 1 + current 1 → fresh");
}

static void ac4_recapture_advances() {
    std::println("\n--- AC4: re-capture advances epoch ---");
    AuraJIT jit;
    jit.capture_fn_epoch("h", 1);
    CHECK(!jit.is_fn_epoch_stale("h", 1), "initial capture fresh");
    jit.capture_fn_epoch("h", 5);
    CHECK(jit.is_fn_epoch_stale("h", 1), "old current (1) is stale after re-capture 5");
    CHECK(!jit.is_fn_epoch_stale("h", 5), "new current (5) is fresh");
}

static void ac5_stress_1000() {
    std::println("\n--- AC5: 1000-iter capture stress ---");
    AuraJIT jit;
    for (std::uint64_t i = 1; i <= 1000; ++i)
        jit.capture_fn_epoch("stress", i);
    CHECK(jit.is_fn_epoch_stale("stress", 999),
          "1000-iter: final epoch 1000 vs current 999 = stale");
    CHECK(!jit.is_fn_epoch_stale("stress", 1000),
          "1000-iter: final epoch 1000 == current 1000 = fresh");
}

static void ac6_capture_bumps_counter() {
    std::println("\n--- AC6: capture_fn_epoch bumps counter ---");
    AuraJIT jit;
    const auto c0 = jit.test_jit_epoch_stale_check_total();
    jit.capture_fn_epoch("a", 1);
    jit.capture_fn_epoch("b", 2);
    jit.capture_fn_epoch("c", 3);
    const auto c1 = jit.test_jit_epoch_stale_check_total();
    CHECK(c1 == c0 + 3, "3 capture_fn_epoch calls bump total by 3");
    std::println("  counter {} → {}", c0, c1);
}

static void ac7_stale_check_is_read() {
    std::println("\n--- AC7: is_fn_epoch_stale is a pure read ---");
    AuraJIT jit;
    jit.capture_fn_epoch("r", 7);
    const auto c0 = jit.test_jit_epoch_stale_check_total();
    for (int i = 0; i < 5; ++i)
        (void)jit.is_fn_epoch_stale("r", static_cast<std::uint64_t>(7 + (i % 2)));
    const auto c1 = jit.test_jit_epoch_stale_check_total();
    CHECK(c1 == c0, "5 is_fn_epoch_stale reads do NOT bump counter");
    std::println("  counter {} → {} (unchanged)", c0, c1);
}

static void ac8_multi_fn_independence() {
    std::println("\n--- AC8: multi-fn independence ---");
    AuraJIT jit;
    for (int i = 0; i < 20; ++i) {
        auto name = std::string("fn") + std::to_string(i);
        jit.capture_fn_epoch(name.c_str(), static_cast<std::uint64_t>(i));
    }
    int stale_n = 0;
    int fresh_n = 0;
    for (int i = 0; i < 20; ++i) {
        auto name = std::string("fn") + std::to_string(i);
        if (jit.is_fn_epoch_stale(name.c_str(), 10))
            ++stale_n;
        else
            ++fresh_n;
    }
    // epoch 10 matches only fn10 → 1 fresh, 19 stale
    CHECK(fresh_n == 1, "exactly one fn matches current epoch 10");
    CHECK(stale_n == 19, "other 19 fns are stale vs epoch 10");
    CHECK(!jit.is_fn_epoch_stale("fn10", 10), "fn10 is fresh at 10");
    CHECK(jit.is_fn_epoch_stale("fn0", 10), "fn0 is stale at 10");
}

} // namespace aura_issue_1477_detail

int main() {
    using namespace aura_issue_1477_detail;
    std::println("=== Issue #1477: JIT dual-epoch fence (re-enabled by #1527) ===");
    ac1_never_captured();
    ac2_capture_same_epoch();
    ac3_capture_then_stale();
    ac4_recapture_advances();
    ac5_stress_1000();
    ac6_capture_bumps_counter();
    ac7_stale_check_is_read();
    ac8_multi_fn_independence();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
