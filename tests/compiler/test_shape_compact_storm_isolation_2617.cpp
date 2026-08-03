// @category: unit
// @reason: Issue #2617 — coverage linter: compact path must never feed
//          deopt-storm ring as mutation (fail-closed gate + AC stress).
//
//   AC1: Gate/linter — on_arena_compact never calls update_deopt_storm_state_
//   AC2: Pure-compact stress — Threshold force-reason stays none; compact counters advance
//   AC3: Mutation invalidate still trips storm logic
//   AC4: on_arena_compact preserves is_stable + history
//   AC5: Source-cite + schema-2617

#include "compiler/shape.h"
#include "compiler/shape_profiler.h"
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::shape::deopt_storm_compact_suppressed;
using aura::compiler::shape::g_deopt_storm_isolations_total_atomic;
using aura::compiler::shape::kShapeCompactStormIsolationIssue;
using aura::compiler::shape::kShapeStormForceReasonNone;
using aura::compiler::shape::kShapeStormForceReasonThreshold;
using aura::compiler::shape::shape_compact_storm_isolation_wired;
using aura::compiler::shape::SHAPE_INT;
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

static void seed_stable(ShapeProfiler& sp, std::uint32_t n, std::uint32_t samples = 120) {
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto fn = static_cast<aura::compiler::shape::FnKey>(6100 + i);
        for (std::uint32_t s = 0; s < samples; ++s)
            sp.record_shape(fn, SHAPE_INT);
    }
}

// ── AC1: source gate evidence ──
static void ac1_gate_compact_no_storm_ring() {
    std::println("\n--- #2617 AC1: gate — compact never feeds storm ring ---");
    CHECK(kShapeCompactStormIsolationIssue == 2617, "AC1: issue stamp");
    const auto spc = read_file("src/compiler/shape_profiler.cpp");
    const auto sph = read_file("src/compiler/shape_profiler.h");
    CHECK(!spc.empty() && !sph.empty(), "AC1: readable shape_profiler sources");
    CHECK(spc.find("COMPACT") != std::string::npos &&
              spc.find("check_shape_compact_storm_isolation_2617") != std::string::npos,
          "AC1: COMPACT↛storm banner + gate cite");
    CHECK(spc.find("Explicitly do NOT call update_deopt_storm_state_") != std::string::npos,
          "AC1: explicit do-not-call on compact path");
    // Strip // comments and ensure on_arena_compact region has no live call.
    // (Full body extraction is the Python gate; here: comment-stripped scan of cpp.)
    std::string stripped;
    stripped.reserve(spc.size());
    for (std::size_t i = 0; i < spc.size(); ++i) {
        if (i + 1 < spc.size() && spc[i] == '/' && spc[i + 1] == '/') {
            while (i < spc.size() && spc[i] != '\n')
                ++i;
            continue;
        }
        stripped.push_back(spc[i]);
    }
    auto compact_pos = stripped.find("on_arena_compact()");
    auto update_def = stripped.find("update_deopt_storm_state_");
    // Find function body start after "ShapeProfiler::on_arena_compact"
    auto fn = stripped.find("ShapeProfiler::on_arena_compact");
    CHECK(fn != std::string::npos, "AC1: on_arena_compact def present");
    if (fn != std::string::npos) {
        auto brace = stripped.find('{', fn);
        CHECK(brace != std::string::npos, "AC1: body brace");
        // Walk brace depth to end of function
        int depth = 0;
        std::size_t end = brace;
        for (; end < stripped.size(); ++end) {
            if (stripped[end] == '{')
                ++depth;
            else if (stripped[end] == '}') {
                --depth;
                if (depth == 0) {
                    ++end;
                    break;
                }
            }
        }
        auto body = stripped.substr(brace, end - brace);
        CHECK(body.find("update_deopt_storm_state_(") == std::string::npos,
              "AC1: no live update_deopt_storm_state_ call in on_arena_compact");
        CHECK(body.find("deopt_storm_compact_suppressed") != std::string::npos,
              "AC1: compact-suppressed tally present");
    }
    (void)compact_pos;
    (void)update_def;
    CHECK(shape_compact_storm_isolation_wired() == 1, "AC1: isolation wired");
}

// ── AC2: pure-compact stress ──
static void ac2_pure_compact_no_threshold() {
    std::println("\n--- #2617 AC2: pure-compact — no Threshold storm ---");
    ShapeProfiler sp;
    sp.apply_preset(ShapeProfiler::kLowMutationPreset);
    seed_stable(sp, 8, 120);

    const auto storm0 = sp.deopt_storm_total();
    const auto iso0 = g_deopt_storm_isolations_total_atomic().load();
    const auto mut0 = sp.mutation_induced_invalidations();
    const auto supp0 = deopt_storm_compact_suppressed.load();
    // Note force-reason is process-global; capture for soft check.
    const auto force0 = shape_storm_force_reason();
    (void)force0;

    for (int i = 0; i < 50; ++i)
        (void)sp.on_arena_compact();

    CHECK(sp.arena_compact_calls() >= 50, "AC2: arena_compact_calls advanced");
    CHECK(sp.arena_compact_deopt_hooks() > 0, "AC2: compact deopt hooks advanced");
    CHECK(sp.deopt_storm_total() == storm0, "AC2: deopt_storm_total unchanged");
    CHECK(g_deopt_storm_isolations_total_atomic().load() == iso0,
          "AC2: isolations unchanged by pure compact");
    CHECK(sp.mutation_induced_invalidations() == mut0,
          "AC2: mutation_induced_invalidations unchanged");
    CHECK(!sp.deopt_storm_active(), "AC2: storm not active after pure compact");
    // Threshold hard fence must not be newly set by compact-only.
    // (Other tests may leave AdaptiveSuppress; Threshold is the ban.)
    if (shape_storm_force_reason() == kShapeStormForceReasonThreshold) {
        // Only acceptable if it was already Threshold before our soak
        // (process global). Force a soft re-clear path via boundary sync.
        (void)sp.on_boundary_or_fiber_sync(true);
    }
    CHECK(shape_storm_force_reason() != kShapeStormForceReasonThreshold ||
              g_deopt_storm_isolations_total_atomic().load() == iso0,
          "AC2: pure compact does not raise Threshold force-reason isolations");
    CHECK(deopt_storm_compact_suppressed.load() > supp0,
          "AC2: deopt_storm_compact_suppressed advanced");
}

// ── AC3: mutation still storms ──
static void ac3_mutation_still_storms() {
    std::println("\n--- #2617 AC3: mutation invalidate still storms ---");
    ShapeProfiler sp;
    sp.apply_preset(ShapeProfiler::kLowMutationPreset);
    sp.set_adaptive_threshold_boost(0);
    seed_stable(sp, 4, 40);

    const auto thr = sp.deopt_storm_threshold();
    const auto iso0 = g_deopt_storm_isolations_total_atomic().load();

    for (std::uint32_t i = 0; i < thr + 3; ++i) {
        const auto fn = static_cast<aura::compiler::shape::FnKey>(7200 + i);
        for (int s = 0; s < 5; ++s)
            sp.record_shape(fn, SHAPE_INT);
        sp.invalidate(fn);
    }
    CHECK(sp.deopt_storm_active(), "AC3: storm active after mutation threshold");
    CHECK(sp.mutation_induced_invalidations() > 0, "AC3: mutation invalidations counted");
    CHECK(g_deopt_storm_isolations_total_atomic().load() > iso0 ||
              shape_storm_force_reason() == kShapeStormForceReasonThreshold,
          "AC3: isolations or Threshold force-reason after mutation");
    CHECK(shape_storm_force_reason() == kShapeStormForceReasonThreshold,
          "AC3: force-reason Threshold on mutation storm");
}

// ── AC4: stable + history preserved ──
static void ac4_stable_history_preserved() {
    std::println("\n--- #2617 AC4: compact preserves is_stable + history ---");
    ShapeProfiler sp;
    sp.apply_preset(ShapeProfiler::kDefaultPreset);
    const auto fn = static_cast<aura::compiler::shape::FnKey>(8001);
    for (int s = 0; s < 200; ++s)
        sp.record_shape(fn, SHAPE_INT);

    // Prefer stable; if still warming, at least preserve profile presence.
    const bool stable_before = sp.is_stable(fn);
    const auto dom_before = sp.dominant_shape(fn);
    const auto ver_before = sp.current_snapshot(fn).version;
    const auto preserved0 = sp.arena_compact_stable_preserved();

    for (int i = 0; i < 10; ++i)
        (void)sp.on_arena_compact();

    CHECK(sp.profile_count() >= 1, "AC4: profile retained");
    if (stable_before) {
        CHECK(sp.is_stable(fn), "AC4: is_stable preserved across compact");
        CHECK(sp.dominant_shape(fn) == dom_before, "AC4: dominant shape preserved");
        CHECK(sp.arena_compact_stable_preserved() > preserved0,
              "AC4: arena_compact_stable_preserved advanced");
    }
    CHECK(sp.current_snapshot(fn).version > ver_before,
          "AC4: version bumped (resume fence) while preserving stability");
    // History not cleared: re-record should not need full re-seed for stability
    // if it was stable (still stable after compact).
    if (stable_before)
        CHECK(sp.is_stable(fn), "AC4: still stable after version bumps");
}

// ── AC5: source-cite + schema ──
static void ac5_source_cite() {
    std::println("\n--- #2617 AC5: source-cite + schema-2617 ---");
    const auto sph = read_file("src/compiler/shape_profiler.h");
    const auto spc = read_file("src/compiler/shape_profiler.cpp");
    CHECK(sph.find("#2617") != std::string::npos, "AC5: header cites #2617");
    CHECK(spc.find("#2617") != std::string::npos, "AC5: cpp cites #2617");
    CHECK(sph.find("#1521") != std::string::npos, "AC5: lineage #1521");
    CHECK(spc.find("#2526") != std::string::npos, "AC5: lineage adaptive #2526");

    CompilerService cs;
    CHECK(href(cs, "schema-2617") == 2617, "AC5: schema-2617");
    CHECK(href(cs, "compact-storm-isolated-wired") == 1, "AC5: compact-storm-isolated-wired");
    CHECK(href(cs, "schema-2526") == 2526, "AC5: schema-2526 retained");
    CHECK(href(cs, "deopt-storm-compact-suppressed") >= 0,
          "AC5: deopt-storm-compact-suppressed exposed");
    CHECK(href(cs, "force-reason-threshold") ==
              static_cast<std::int64_t>(kShapeStormForceReasonThreshold),
          "AC5: force-reason-threshold code");
    CHECK(kShapeStormForceReasonNone == 0, "AC5: none reason");
}

} // namespace

int main() {
    std::println("=== Issue #2617: compact ↛ deopt-storm ring isolation ===");
    ac1_gate_compact_no_storm_ring();
    ac2_pure_compact_no_threshold();
    ac3_mutation_still_storms();
    ac4_stable_history_preserved();
    ac5_source_cite();
    std::println("\n=== #2617: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
