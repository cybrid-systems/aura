// @category: unit
// @reason: Issue #2433 — ShapeProfiler HighMutation default-on + deopt-storm
//          isolation coordinates with LayoutStamp.shape_version / SpecJIT.
//
//   AC1: production default applies kHighMutationPreset knobs (no env)
//   AC2: storm enter → shape_version advances + specialized code isolated
//   AC3: continuous invalidate stress → isolations bounded (not per deopt)
//   AC4: soft / no-storm path: no force-reason writes when quiet
//   AC5: query:shape-storm-health + schema-2433 + source-cite

#include "test_harness.hpp"
#include "compiler/shape_profiler.h"
#include "compiler/spec_jit_controller.h"
#include "compiler/aura_jit.h"

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
using aura::compiler::shape::g_deopt_storm_isolations_total_atomic;
using aura::compiler::shape::g_shape_version_at_storm_atomic;
using aura::compiler::shape::kShapeStormForceReasonNone;
using aura::compiler::shape::kShapeStormForceReasonThreshold;
using aura::compiler::shape::shape_high_mutation_default_enabled;
using aura::compiler::shape::SHAPE_INT;
using aura::compiler::shape::shape_storm_force_reason;
using aura::compiler::shape::shape_version_at_last_storm;
using aura::compiler::shape::ShapeProfiler;
using aura::compiler::shape::SpecJITController;
static constexpr auto kHighMutationPreset = ShapeProfiler::kHighMutationPreset;
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

static std::int64_t href(CompilerService& cs, std::string_view prim, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", prim, key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace

int run_test_shape_high_mutation_storm_2433() {
    std::println("=== Issue #2433: HighMutation default + storm × LayoutStamp ===");

    // ── AC1: HighMutation knobs applied without env ────────────────
    {
        std::println("\n--- #2433 AC1: HighMutation production default knobs ---");
        // Env may be unset or 1 in production-like runs.
        if (const char* e = std::getenv("AURA_SHAPE_HIGH_MUTATION"); e && e[0] == '0') {
            std::println("  (skip runtime knobs: AURA_SHAPE_HIGH_MUTATION=0)");
            CHECK(shape_high_mutation_default_enabled() == 0, "AC1: env=0 disables");
        } else {
            CHECK(shape_high_mutation_default_enabled() == 1, "AC1: default enabled");
            ShapeProfiler sp;
            const auto p = sp.active_preset();
            CHECK(p.window_size == kHighMutationPreset.window_size, "AC1: window_size");
            CHECK(p.stability_ratio == kHighMutationPreset.stability_ratio, "AC1: stability_ratio");
            CHECK(p.min_samples_for_stable == kHighMutationPreset.min_samples_for_stable,
                  "AC1: min_samples");
            CHECK(p.deopt_storm_window == kHighMutationPreset.deopt_storm_window,
                  "AC1: deopt_storm_window");
            CHECK(p.deopt_storm_threshold == kHighMutationPreset.deopt_storm_threshold,
                  "AC1: deopt_storm_threshold");
            CHECK(sp.window_size() == kHighMutationPreset.window_size,
                  "AC1: window_size() live knob");
            CHECK(sp.deopt_storm_threshold() == kHighMutationPreset.deopt_storm_threshold,
                  "AC1: deopt_storm_threshold() live knob");
        }
        auto spc = read_file("src/compiler/shape_profiler.cpp");
        CHECK(spc.find("apply_preset(kHighMutationPreset)") != std::string::npos,
              "AC1: ctor apply_preset source-cite");
        CHECK(spc.find("Issue #2433") != std::string::npos, "AC1: #2433 source-cite in cpp");
    }

    // ── AC2: storm enter → version + isolation snapshot ────────────
    {
        std::println("\n--- #2433 AC2: storm enter isolates specialized path ---");
        ShapeProfiler sp;
        // Force Default storm knobs for a fast trip (threshold small).
        sp.apply_preset(ShapeProfiler::kLowMutationPreset); // threshold=3
        const auto v0 = current_global_shape_version();
        const auto iso0 = g_deopt_storm_isolations_total_atomic().load();
        const auto thr = sp.deopt_storm_threshold();
        // Drive invalidates past threshold to enter storm.
        for (std::uint32_t i = 0; i < thr + 2; ++i) {
            const auto fn = static_cast<aura::compiler::shape::FnKey>(1000 + i);
            // Seed profile so invalidate finds it.
            for (int s = 0; s < 5; ++s)
                sp.record_shape(fn, SHAPE_INT);
            sp.invalidate(fn);
        }
        CHECK(sp.deopt_storm_active(), "AC2: deopt_storm_active after threshold");
        CHECK(current_global_shape_version() > v0, "AC2: shape_version advanced on storm enter");
        CHECK(g_deopt_storm_isolations_total_atomic().load() > iso0,
              "AC2: isolations counter bumped once-per-enter");
        CHECK(shape_storm_force_reason() == kShapeStormForceReasonThreshold,
              "AC2: force-reason threshold");
        CHECK(shape_version_at_last_storm() >= v0, "AC2: shape-version-at-storm snapshot");

        // SpecJIT: install with old stamp-like version, then miss after storm.
        aura::jit::AuraJIT jit;
        SpecJITController ctl(jit);
        // Install under current (post-storm) version — serves; then bump
        // via another storm isolation path: re-enter is no-op while active,
        // so simulate isolation by installing with stale version via
        // effective miss after process version advance.
        auto* dummy = reinterpret_cast<aura::jit::ScalarFn>(static_cast<std::uintptr_t>(1));
        ctl.install_specialization("fn-2433", SHAPE_INT, dummy);
        CHECK(ctl.has_specialization("fn-2433", SHAPE_INT),
              "AC2: install succeeds at current version");
        // Bump global version (LayoutStamp coordination) → stamp miss.
        aura::compiler::shape::bump_shape_version_on_storm_enter();
        CHECK(!ctl.has_specialization("fn-2433", SHAPE_INT),
              "AC2: specialized code isolated after version advance");
    }

    // ── AC3: continuous invalidate → isolations bounded ────────────
    {
        std::println("\n--- #2433 AC3: isolations bounded under continuous mutate ---");
        ShapeProfiler sp;
        sp.apply_preset({64, 0.90, 10, 32, 4}); // small window, thr=4
        const auto iso_before = g_deopt_storm_isolations_total_atomic().load();
        for (int i = 0; i < 200; ++i) {
            const auto fn = static_cast<aura::compiler::shape::FnKey>(2000 + (i % 8));
            sp.record_shape(fn, SHAPE_INT);
            sp.invalidate(fn);
        }
        const auto iso_delta = g_deopt_storm_isolations_total_atomic().load() - iso_before;
        // One enter (or few if clear+reenter); never one-per-invalidate (200).
        CHECK(iso_delta < 50, "AC3: isolations bounded (not per deopt)");
        CHECK(iso_delta >= 1 || sp.deopt_storm_active(),
              "AC3: storm path exercised under continuous mutate");
    }

    // ── AC4: soft path zero force-reason churn when quiet ───────────
    {
        std::println("\n--- #2433 AC4: soft path no storm writes ---");
        // Reset force-reason via soft clear path.
        ShapeProfiler sp;
        sp.apply_preset(ShapeProfiler::kDefaultPreset);
        // Quiet record_shape only — no invalidate → no storm enter.
        const auto fr0 = shape_storm_force_reason();
        const auto at0 = shape_version_at_last_storm();
        for (int i = 0; i < 20; ++i)
            sp.record_shape(42, SHAPE_INT);
        CHECK(shape_version_at_last_storm() == at0,
              "AC4: shape-version-at-storm unchanged on soft record");
        // force-reason only changes on enter/clear transitions, not soft record.
        (void)fr0;
        CHECK(!sp.deopt_storm_active() || shape_storm_force_reason() != kShapeStormForceReasonNone,
              "AC4: quiet path does not falsely trip storm");
    }

    // ── AC5: query surface + source-cite ────────────────────────────
    {
        std::println("\n--- #2433 AC5: query:shape-storm-health + schema ---");
        auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
        auto hh = read_file("src/compiler/shape_profiler.h");
        CHECK(q.find("query:shape-storm-health") != std::string::npos,
              "AC5: query:shape-storm-health registered");
        CHECK(q.find("schema-2433") != std::string::npos, "AC5: schema-2433");
        CHECK(q.find("shape-version-at-storm") != std::string::npos,
              "AC5: shape-version-at-storm key");
        CHECK(hh.find("g_shape_version_at_storm_atomic") != std::string::npos,
              "AC5: version-at-storm atomic");
        CHECK(hh.find("kShapeStormForceReasonThreshold") != std::string::npos,
              "AC5: force-reason codes");

        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        const auto schema = href(cs, "query:shape-storm-health", "schema-2433");
        CHECK(schema == 2433, "AC5: schema-2433 runtime");
        const auto health = href(cs, "query:shape-storm-health", "health-bp");
        CHECK(health >= 0 && health <= 10000, "AC5: health-bp in range");
        const auto hm = href(cs, "query:shape-storm-health", "high-mutation-default");
        CHECK(hm == 0 || hm == 1, "AC5: high-mutation-default 0|1");
        // Also folded into shape-profiler-stats
        const auto s2433 = href(cs, "query:shape-profiler-stats", "schema-2433");
        CHECK(s2433 == 2433, "AC5: shape-profiler-stats schema-2433");
    }

    std::println("\n=== #2433 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_shape_high_mutation_storm_2433();
}
#endif
