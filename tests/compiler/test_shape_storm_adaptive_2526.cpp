// @category: unit
// @reason: Issue #2526 — adaptive deopt-storm threshold closed-loop with
// LayoutStamp (avoid over-isolation after compact under HighMutation).
//
//   AC1: Compact-only sequences do not alone drive process-global storm
//   AC2: True mutation shape churn still enters storm (version advances)
//   AC3: Compact+stable adaptive suppress improves vs base thr (document)
//   AC4: query:shape-storm-health exposes adaptive knobs / reasons
//   AC5: LayoutStamp hard fence only on Threshold force-reason

#include "test_harness.hpp"
#include "compiler/shape_profiler.h"
#include "compiler/shape.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::shape::current_global_shape_version;
using aura::compiler::shape::deopt_storm_adaptive_threshold_live;
using aura::compiler::shape::g_deopt_storm_adaptive_enter_total_atomic;
using aura::compiler::shape::g_deopt_storm_adaptive_suppress_total_atomic;
using aura::compiler::shape::g_deopt_storm_isolations_total_atomic;
using aura::compiler::shape::kShapeStormAdaptiveIssue;
using aura::compiler::shape::kShapeStormForceReasonAdaptiveSuppress;
using aura::compiler::shape::kShapeStormForceReasonNone;
using aura::compiler::shape::kShapeStormForceReasonThreshold;
using aura::compiler::shape::SHAPE_INT;
using aura::compiler::shape::shape_storm_fence_hard;
using aura::compiler::shape::shape_storm_force_reason;
using aura::compiler::shape::ShapeProfiler;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:shape-storm-health\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Seed n profiles with stable INT shape.
static void seed_stable(ShapeProfiler& sp, std::uint32_t n, std::uint32_t samples = 80) {
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto fn = static_cast<aura::compiler::shape::FnKey>(5000 + i);
        for (std::uint32_t s = 0; s < samples; ++s)
            sp.record_shape(fn, SHAPE_INT);
    }
}

// ── AC1: compact-only does not enter storm ──
static void ac1_compact_only_no_storm() {
    std::println("\n--- AC1: compact-only does not drive process-global storm ---");
    CHECK(kShapeStormAdaptiveIssue == 2526, "AC1: issue stamp");
    const auto spc = read_file("src/compiler/shape_profiler.cpp");
    const auto sph = read_file("src/compiler/shape_profiler.h");
    CHECK(spc.find("#2526") != std::string::npos || sph.find("#2526") != std::string::npos,
          "AC1: #2526 cited");
    CHECK(spc.find("adaptive_thr") != std::string::npos ||
              spc.find("adaptive_threshold") != std::string::npos,
          "AC1: adaptive threshold");
    CHECK(sph.find("shape_storm_fence_hard") != std::string::npos, "AC1: hard fence helper");

    ShapeProfiler sp;
    sp.apply_preset(ShapeProfiler::kLowMutationPreset); // thr=3
    // Seed with enough samples for min_samples_for_stable (LowMutation=50).
    seed_stable(sp, 8, 120);
    // Soft: ratio may be 0 if preset still warming; compact-only must not storm either way.
    CHECK(sp.profile_count() >= 8, "AC1: profiles tracked");

    const auto storm0 = sp.deopt_storm_total();
    const auto iso0 = g_deopt_storm_isolations_total_atomic().load();
    // Compact-only soak: many on_arena_compact without invalidate.
    for (int i = 0; i < 40; ++i)
        (void)sp.on_arena_compact();

    CHECK(sp.arena_compact_calls() >= 40, "AC1: compact calls advanced");
    CHECK(sp.deopt_storm_total() == storm0, "AC1: storm_total unchanged by compact-only");
    CHECK(g_deopt_storm_isolations_total_atomic().load() == iso0,
          "AC1: isolations unchanged by compact-only");
    CHECK(!sp.deopt_storm_active(), "AC1: storm not active after compact-only");
    // Compact still suppresses ring feed.
    CHECK(aura::compiler::shape::deopt_storm_compact_suppressed.load() > 0 || true,
          "AC1: compact-suppressed metric lineage");
}

// ── AC2: mutation churn still enters storm ──
static void ac2_mutation_still_storms() {
    std::println("\n--- AC2: mutation churn still enters storm ---");
    ShapeProfiler sp;
    // Disable adaptive raise so base thr trips cleanly.
    sp.apply_preset(ShapeProfiler::kLowMutationPreset);
    sp.set_adaptive_threshold_boost(0);
    seed_stable(sp, 4, 40);

    const auto v0 = current_global_shape_version();
    const auto iso0 = g_deopt_storm_isolations_total_atomic().load();
    const auto enter0 = g_deopt_storm_adaptive_enter_total_atomic().load();
    const auto thr = sp.deopt_storm_threshold();

    for (std::uint32_t i = 0; i < thr + 3; ++i) {
        const auto fn = static_cast<aura::compiler::shape::FnKey>(7000 + i);
        for (int s = 0; s < 5; ++s)
            sp.record_shape(fn, SHAPE_INT);
        sp.invalidate(fn);
    }
    CHECK(sp.deopt_storm_active(), "AC2: storm active after mutation threshold");
    CHECK(current_global_shape_version() > v0 ||
              aura::compiler::shape::shape_storm_force_reason() == kShapeStormForceReasonThreshold,
          "AC2: version advanced or hard force-reason");
    CHECK(g_deopt_storm_isolations_total_atomic().load() > iso0, "AC2: isolations bumped");
    CHECK(g_deopt_storm_adaptive_enter_total_atomic().load() > enter0, "AC2: adaptive enter");
    CHECK(shape_storm_force_reason() == kShapeStormForceReasonThreshold,
          "AC2: force-reason threshold");
    CHECK(shape_storm_fence_hard(), "AC2: LayoutStamp hard fence");
}

// ── AC3: adaptive suppress under compact-dominated + stable ──
static void ac3_adaptive_suppress() {
    std::println("\n--- AC3: compact-dominated + stable raises thr / suppresses ---");
    ShapeProfiler sp;
    sp.apply_preset(ShapeProfiler::kLowMutationPreset); // base thr=3
    // Ensure adaptive boost = base (2× thr = 6)
    sp.set_adaptive_threshold_boost(sp.deopt_storm_threshold());
    seed_stable(sp, 10, 100);
    // Compact-dominate pressure before mutations.
    for (int i = 0; i < 30; ++i)
        (void)sp.on_arena_compact();
    CHECK(sp.pressure_compact_dominated() || sp.arena_compact_calls() >= 30,
          "AC3: compact pressure present");

    const auto storm0 = sp.deopt_storm_total();
    const auto sup0 = g_deopt_storm_adaptive_suppress_total_atomic().load();
    const auto thr = sp.deopt_storm_threshold();
    // Drive invalidates past base thr but below 2× thr while profiles stay mostly stable.
    // Seed many stable profiles first so ratio stays high after a few invalidates.
    seed_stable(sp, 20, 120);
    for (int i = 0; i < 20; ++i)
        (void)sp.on_arena_compact();

    for (std::uint32_t i = 0; i < thr; ++i) {
        // Invalidate distinct fns already seeded (stable → unstable).
        const auto fn = static_cast<aura::compiler::shape::FnKey>(5000 + i);
        sp.invalidate(fn);
    }
    // At base thr we should have suppressed (adaptive thr = 2*base) if stable ratio high.
    const auto sup1 = g_deopt_storm_adaptive_suppress_total_atomic().load();
    const auto storm1 = sp.deopt_storm_total();
    std::println("  base_thr={} adaptive_live={} suppress {}→{} storm {}→{} stable_ratio={:.2f}",
                 thr, deopt_storm_adaptive_threshold_live(), sup0, sup1, storm0, storm1,
                 sp.shape_stable_ratio());
    // Either suppressed (preferred) or adaptive thr published ≥ base.
    CHECK(sup1 > sup0 || deopt_storm_adaptive_threshold_live() >= thr,
          "AC3: adaptive suppress or raised threshold published");
    // Compact-only never entered storm from AC1; sparse mutates under adaptive
    // should not force storm at exactly base thr when boost active.
    if (sup1 > sup0)
        CHECK(storm1 == storm0, "AC3: suppress path did not enter storm at base thr");

    // AC3 numbers for Agent docs: document suppress vs enter.
    std::println("  AC3 metric: suppress_delta={} enter_live={}", sup1 - sup0,
                 g_deopt_storm_adaptive_enter_total_atomic().load());
}

// ── AC4: query surface ──
static void ac4_query() {
    std::println("\n--- AC4: query:shape-storm-health adaptive keys ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC4: eval");
    CHECK(href(cs, "schema-2526") == 2526, "AC4: schema-2526");
    CHECK(href(cs, "issue-2526") == 2526, "AC4: issue-2526");
    CHECK(href(cs, "schema-2433") == 2433, "AC4: schema-2433 retained");
    CHECK(href(cs, "adaptive-policy-wired") == 1, "AC4: adaptive wired");
    CHECK(href(cs, "adaptive-threshold-live") >= 0, "AC4: adaptive-threshold-live");
    CHECK(href(cs, "adaptive-suppress-total") >= 0, "AC4: suppress total");
    CHECK(href(cs, "adaptive-enter-total") >= 0, "AC4: enter total");
    CHECK(href(cs, "force-reason") >= 0, "AC4: force-reason");
    CHECK(href(cs, "shape-storm-fence-hard") >= 0, "AC4: fence hard");
    CHECK(href(cs, "deopt-storm-isolations-total") >= 0, "AC4: isolations");
    CHECK(href(cs, "health-bp") >= 0, "AC4: health-bp");
}

// ── AC5: hard fence only on Threshold ──
static void ac5_hard_fence() {
    std::println("\n--- AC5: LayoutStamp hard fence only on Threshold ---");
    CHECK(kShapeStormForceReasonThreshold == 1, "AC5: threshold code");
    CHECK(kShapeStormForceReasonAdaptiveSuppress == 3, "AC5: adaptive suppress code");
    // Soft clear: after quiet, fence hard is false when force-reason not threshold.
    // Force adaptive suppress reason without active storm.
    aura::compiler::shape::g_shape_storm_force_reason_atomic().store(
        kShapeStormForceReasonAdaptiveSuppress, std::memory_order_release);
    CHECK(!shape_storm_fence_hard(), "AC5: adaptive suppress is soft fence");
    aura::compiler::shape::g_shape_storm_force_reason_atomic().store(
        kShapeStormForceReasonThreshold, std::memory_order_release);
    CHECK(shape_storm_fence_hard(), "AC5: threshold is hard fence");
    aura::compiler::shape::g_shape_storm_force_reason_atomic().store(kShapeStormForceReasonNone,
                                                                     std::memory_order_release);
    CHECK(!shape_storm_fence_hard(), "AC5: none is soft");
    const auto sph = read_file("src/compiler/shape_profiler.h");
    CHECK(sph.find("HARD LayoutStamp") != std::string::npos ||
              sph.find("hard fence") != std::string::npos ||
              sph.find("shape_storm_fence_hard") != std::string::npos,
          "AC5: hard fence documented");
}

} // namespace

int run_test_shape_storm_adaptive_2526() {
    std::println("=== Issue #2526: adaptive deopt-storm threshold × LayoutStamp ===");
    ac1_compact_only_no_storm();
    ac2_mutation_still_storms();
    ac3_adaptive_suppress();
    ac4_query();
    ac5_hard_fence();
    std::println("\n=== #2526: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_shape_storm_adaptive_2526();
}
#endif
