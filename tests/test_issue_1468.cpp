// @category: unit
// @reason: pure C++ ShapeProfiler; no CompilerService / LLVM JIT
//
// test_issue_1468.cpp — Issue #1468: ShapeProfiler history/dominant/stability
// tuning + deopt-storm protection + AI workload metrics.
//
// ACs:
//   AC1: Preset application (kDefaultPreset / kHighMutationPreset / kLowMutationPreset)
//   AC2: Deopt-storm detection: N deopts in M calls → deopt_storm_active() == true
//   AC3: 4 AI metrics: shape_stable_ratio / deopt_rate_per_fn /
//        history_hit_rate / mutation_induced_invalidations
//   AC4: invalidate_all bumps mutation_induced_invalidations_ by profile count
//   AC5: history_hit_count_ bumps per record_shape
//   AC6: SpecJITController coordination hook (deopt-storm aware)

#include "test_harness.hpp"

#include "compiler/shape_profiler.h"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

namespace test_issue_1468_detail {

#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::println("  FAIL: {} (line {})", msg, __LINE__);                                   \
            ++g_failed;                                                                            \
        } else {                                                                                   \
            std::println("  PASS: {}", msg);                                                       \
            ++g_passed;                                                                            \
        }                                                                                          \
    } while (0)

using aura::compiler::shape::SHAPE_INT;
using aura::compiler::shape::ShapeID;
using aura::compiler::shape::ShapeProfiler;

void ac1_preset_application() {
    std::println("\n--- AC1: Preset application ---");
    ShapeProfiler sp;
    sp.apply_preset(ShapeProfiler::kHighMutationPreset);
    const auto active = sp.active_preset();
    CHECK(active.window_size == ShapeProfiler::kHighMutationPreset.window_size,
          "high-mutation preset window_size applied");
    CHECK(active.stability_ratio == ShapeProfiler::kHighMutationPreset.stability_ratio,
          "high-mutation preset stability_ratio applied");
    CHECK(active.deopt_storm_window == ShapeProfiler::kHighMutationPreset.deopt_storm_window,
          "high-mutation preset deopt_storm_window applied");
    CHECK(active.deopt_storm_threshold == ShapeProfiler::kHighMutationPreset.deopt_storm_threshold,
          "high-mutation preset deopt_storm_threshold applied");
    sp.apply_preset(ShapeProfiler::kLowMutationPreset);
    CHECK(sp.active_preset().window_size == ShapeProfiler::kLowMutationPreset.window_size,
          "low-mutation preset window_size applied");
    sp.apply_preset(ShapeProfiler::kDefaultPreset);
    CHECK(sp.active_preset().window_size == ShapeProfiler::kDefaultPreset.window_size,
          "default preset round-trip");
}

void ac2_deopt_storm_detection() {
    std::println("\n--- AC2: Deopt-storm detection ---");
    ShapeProfiler sp;
    sp.apply_preset(ShapeProfiler::kDefaultPreset); // 256 window, 4 threshold
    CHECK(!sp.deopt_storm_active(), "deopt_storm starts inactive");
    // Trigger 4 deopts on the same fn (default threshold). The default
    // preset's threshold is 4 — 4 deopts should activate the storm.
    for (int i = 0; i < 4; ++i) {
        sp.record_shape(1, SHAPE_INT);
        (void)sp.invalidate(1);
    }
    CHECK(sp.deopt_storm_active(), "4 deopts in default window activates deopt_storm");
    CHECK(sp.deopt_storm_total() >= 1, "deopt_storm_total counter bumped on first activation");
}

void ac3_metrics_initial_state() {
    std::println("\n--- AC3: 4 AI metrics initial state ---");
    ShapeProfiler sp;
    CHECK(sp.shape_stable_ratio() == 0.0, "shape_stable_ratio == 0 with no profiles");
    CHECK(sp.deopt_rate_per_fn() == 0.0, "deopt_rate_per_fn == 0 with no profiles");
    CHECK(sp.history_hit_rate() == 0.0, "history_hit_rate == 0 with no history");
    CHECK(sp.mutation_induced_invalidations() == 0,
          "mutation_induced_invalidations == 0 initially");
    CHECK(sp.history_hit_count() == 0, "history_hit_count == 0 initially");
    CHECK(sp.history_miss_count() == 0, "history_miss_count == 0 initially");
    CHECK(sp.deopt_storm_total() == 0, "deopt_storm_total == 0 initially");
}

void ac4_invalidate_all_counter() {
    std::println("\n--- AC4: invalidate_all bumps mutation_induced_invalidations ---");
    ShapeProfiler sp;
    // Seed profiles.
    for (std::uint64_t fn = 1; fn <= 5; ++fn) {
        sp.record_shape(static_cast<aura::compiler::shape::FnKey>(fn), SHAPE_INT);
        // Push enough samples to potentially reach stability threshold;
        // we don't need it stable for this AC, just present.
        for (int i = 0; i < 200; ++i)
            sp.record_shape(static_cast<aura::compiler::shape::FnKey>(fn), SHAPE_INT);
    }
    const auto before = sp.mutation_induced_invalidations();
    sp.invalidate_all();
    const auto after = sp.mutation_induced_invalidations();
    CHECK(after == before + 5,
          "invalidate_all() bumps mutation_induced_invalidations by profile count (5)");
}

void ac5_history_hit_counter() {
    std::println("\n--- AC5: history_hit_count bumps per record_shape ---");
    ShapeProfiler sp;
    const auto before = sp.history_hit_count();
    for (int i = 0; i < 100; ++i) {
        sp.record_shape(1, SHAPE_INT);
    }
    const auto after = sp.history_hit_count();
    CHECK(after == before + 100, "history_hit_count bumps by 100 after 100 record_shape calls");
    CHECK(sp.history_hit_rate() > 0.0, "history_hit_rate > 0 after history lookups");
}

void ac6_spec_jit_controller_awareness() {
    std::println("\n--- AC6: SpecJITController coordination via deopt_storm_active ---");
    // The SpecJITController is supposed to query deopt_storm_active()
    // and down-shift to generic. We can't easily wire SpecJITController
    // here without LLVM JIT, but we verify the contract: the flag is
    // observable + persists until reset.
    ShapeProfiler sp;
    sp.apply_preset(ShapeProfiler::kDefaultPreset); // threshold 4
    CHECK(!sp.deopt_storm_active(), "starts inactive");
    for (int i = 0; i < 10; ++i) {
        sp.record_shape(2, SHAPE_INT);
        (void)sp.invalidate(2);
    }
    CHECK(sp.deopt_storm_active(), "storm active after many deopts (callers can downshift)");
    // reset() should clear the storm (verified via profile eviction path
    // tested elsewhere — here we just check the accessor works).
    CHECK(sp.deopt_storm_total() >= 1, "storm bump count recorded");
}

} // namespace test_issue_1468_detail

int main() {
    using namespace test_issue_1468_detail;
    std::println("=== Issue #1468 — ShapeProfiler tuning + deopt-storm + AI metrics ===");
    ac1_preset_application();
    ac2_deopt_storm_detection();
    ac3_metrics_initial_state();
    ac4_invalidate_all_counter();
    ac5_history_hit_counter();
    ac6_spec_jit_controller_awareness();

    std::println("\n─── #1468 summary: {}/{} passed, {}/{} failed ───", g_passed,
                 g_passed + g_failed, g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}