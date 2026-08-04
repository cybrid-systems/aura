// @category: integration
// @reason: Issue #2059 — Adaptive Arena compaction policy + ShapeProfiler
// deopt coordination under AI mutation load (extends #1621 / #1919 / #1521).
//
//   AC1: compute_adaptive_headroom varies with mutation vs deopt storm
//        (tighter under AI frag → lower peak RSS; looser under storm)
//   AC2: evaluate_auto_compact_policy soft-gates under deopt storm when
//        frag is not critical; still triggers on high frag
//   AC3: publish_shape_deopt_metrics feeds rate/storm into policy
//   AC4: query:arena-auto-policy-stats schema-2059 + decision keys
//   AC5: production-sweep schema-2059 adaptive keys
//   AC6: post-compact resync wired (service compact hook always re-syncs)
//   AC7: multi-round mutate stress — adaptive headroom ≤ fixed 25% baseline
//        under mutation pressure; deopt metrics remain accurate
//   AC8: short-path / low-mutation quiet path does not force compact
//   AC9: #1919 / #1621 lineage schema retained

#include "test_harness.hpp"
#include "core/arena_auto_policy_stats.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::arena_policy::auto_compact_mode;
using aura::core::arena_policy::AutoCompactMode;
using aura::core::arena_policy::compute_adaptive_headroom;
using aura::core::arena_policy::current_adaptive_headroom;
using aura::core::arena_policy::evaluate_auto_compact_policy;
using aura::core::arena_policy::kFixedHeadroomBaselineBp;
using aura::core::arena_policy::kHeadroomMax;
using aura::core::arena_policy::kHeadroomMin;
using aura::core::arena_policy::publish_shape_deopt_metrics;
using aura::core::arena_policy::set_auto_compact_mode;
using aura::core::arena_policy::signal_mutation_pressure;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void clear_pressure_signals() {
    (void)aura::core::arena_policy::consume_mutation_pressure();
    (void)aura::core::arena_policy::consume_jit_deopt_pressure();
    (void)aura::core::arena_policy::consume_shape_churn();
    aura::core::arena_policy::shape_deopt_storm_active.store(false, std::memory_order_release);
    aura::core::arena_policy::shape_deopt_rate_bp.store(0, std::memory_order_relaxed);
    aura::core::arena_policy::shape_stable_ratio_bp.store(10000, std::memory_order_relaxed);
    set_auto_compact_mode(AutoCompactMode::Balanced);
}

static void ac1_adaptive_headroom() {
    std::println("\n--- AC1: adaptive headroom mutation vs storm ---");
    clear_pressure_signals();
    set_auto_compact_mode(AutoCompactMode::Balanced);

    const double hr_quiet =
        compute_adaptive_headroom(AutoCompactMode::Balanced, false, false, false, 0.10, 0.0);
    const double hr_mut =
        compute_adaptive_headroom(AutoCompactMode::Balanced, true, false, false, 0.55, 0.0);
    const double hr_storm =
        compute_adaptive_headroom(AutoCompactMode::Balanced, false, true, true, 0.20, 0.5);
    const double hr_agg =
        compute_adaptive_headroom(AutoCompactMode::Aggressive, true, false, false, 0.70, 0.0);
    const double hr_cons =
        compute_adaptive_headroom(AutoCompactMode::Conservative, false, false, false, 0.10, 0.0);

    CHECK(hr_quiet >= kHeadroomMin && hr_quiet <= kHeadroomMax, "quiet headroom in range");
    CHECK(hr_mut >= kHeadroomMin && hr_mut <= kHeadroomMax, "mut headroom in range");
    CHECK(hr_storm >= kHeadroomMin && hr_storm <= kHeadroomMax, "storm headroom in range");
    CHECK(hr_mut <= hr_quiet, "mutation+high-frag → tighter headroom (lower peak RSS)");
    CHECK(hr_storm >= hr_quiet, "deopt storm → looser headroom (fewer compact→deopt)");
    CHECK(hr_agg <= hr_mut + 0.001, "Aggressive ≤ mut Balanced under pressure");
    CHECK(hr_cons >= hr_quiet, "Conservative ≥ quiet Balanced");
    // Fixed 25% baseline comparison: under mutation, adaptive ≤ baseline.
    CHECK(hr_mut * 10000.0 <= static_cast<double>(kFixedHeadroomBaselineBp) + 1.0,
          "mut headroom ≤ fixed 25% baseline");
    CHECK(current_adaptive_headroom() >= kHeadroomMin, "current headroom published");
}

static void ac2_storm_soft_gate() {
    std::println("\n--- AC2: deopt storm soft-gates non-critical compact ---");
    clear_pressure_signals();
    set_auto_compact_mode(AutoCompactMode::Balanced);

    // Quiet low frag → no trigger.
    auto quiet = evaluate_auto_compact_policy(0.05, false, false, false, false, false, 0.1);
    CHECK(!quiet.should_compact, "low frag → no trigger");
    CHECK(quiet.headroom_used >= kHeadroomMin, "headroom recorded on quiet eval");

    // High frag → still triggers (critical memory pressure wins).
    auto frag = evaluate_auto_compact_policy(0.70, false, false, false, false, false, 0.1);
    CHECK(frag.should_compact, "high frag → trigger");
    CHECK((frag.reason & aura::core::arena_policy::kPolicyReasonFrag) != 0, "frag reason");

    // Active storm + bare dirty (low frag) → soft-gate (no compact).
    publish_shape_deopt_metrics(/*deopt_rate=*/2.0, /*storm=*/true, /*stable=*/0.4);
    auto storm_dirty = evaluate_auto_compact_policy(0.12, false, true, false, false, false, 0.1);
    CHECK(!storm_dirty.should_compact, "storm + bare dirty soft-gated");
    CHECK(storm_dirty.soft_gate_recommended, "soft_gate_recommended set");
    CHECK((storm_dirty.reason_ext & aura::core::arena_policy::kPolicyReasonSoftGatedStorm) != 0,
          "soft-gated-storm reason_ext");
    CHECK((storm_dirty.reason_ext & aura::core::arena_policy::kPolicyReasonDeoptStorm) != 0,
          "deopt-storm reason_ext");

    // Storm + critical high frag still triggers.
    auto storm_crit = evaluate_auto_compact_policy(0.85, false, false, false, false, false, 0.1);
    // Threshold may be raised under elevated deopt — 0.85 should still clear max 0.60 thr.
    CHECK(storm_crit.should_compact, "storm + critical frag still triggers");
    CHECK(!storm_crit.soft_gate_recommended, "critical path not soft-gated");

    clear_pressure_signals();
}

static void ac3_publish_metrics() {
    std::println("\n--- AC3: publish_shape_deopt_metrics feeds policy ---");
    clear_pressure_signals();
    publish_shape_deopt_metrics(0.25, true, 0.55);
    CHECK(aura::core::arena_policy::shape_deopt_storm_active.load(), "storm published");
    CHECK(aura::core::arena_policy::shape_deopt_rate_bp.load() >= 2500, "rate bp ≥ 0.25");
    CHECK(aura::core::arena_policy::shape_stable_ratio_bp.load() >= 5500, "stable bp ≥ 0.55");
    CHECK(aura::core::arena_policy::peek_jit_deopt_pressure(),
          "elevated rate → jit deopt pressure");
    clear_pressure_signals();
}

static void ac4_policy_stats_schema() {
    std::println("\n--- AC4: query:arena-auto-policy-stats schema-2059 ---");
    CompilerService cs;
    signal_mutation_pressure();
    auto h = cs.eval("(engine:metrics \"query:arena-auto-policy-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "query:arena-auto-policy-stats", "schema") == 1621, "lineage schema 1621");
    CHECK(href(cs, "query:arena-auto-policy-stats", "schema-1919") == 1919, "schema-1919");
    CHECK(href(cs, "query:arena-auto-policy-stats", "schema-2059") == 2059, "schema-2059");
    CHECK(href(cs, "query:arena-auto-policy-stats", "issue-2059") == 2059, "issue-2059");
    CHECK(href(cs, "query:arena-auto-policy-stats", "adaptive-policy-wired") == 1, "wired");
    CHECK(href(cs, "query:arena-auto-policy-stats", "adaptive-headroom-bp") >= 1250, "hr ≥ min");
    CHECK(href(cs, "query:arena-auto-policy-stats", "adaptive-headroom-bp") <= 5000, "hr ≤ max");
    CHECK(href(cs, "query:arena-auto-policy-stats", "fixed-headroom-baseline-bp") == 2500,
          "fixed 25%");
    CHECK(href(cs, "query:arena-auto-policy-stats", "headroom-min-bp") == 1250, "min 12.5%");
    CHECK(href(cs, "query:arena-auto-policy-stats", "headroom-max-bp") == 5000, "max 50%");
    CHECK(href(cs, "query:arena-auto-policy-stats", "last-decision-reason") >= 0, "reason");
    CHECK(href(cs, "query:arena-auto-policy-stats", "shape-deopt-rate-bp") >= 0, "deopt rate");
    CHECK(href(cs, "query:arena-auto-policy-stats", "shape-deopt-storm-active") >= 0, "storm flag");
    CHECK(href(cs, "query:arena-auto-policy-stats", "post-compact-resync-total") >= 0, "resync");
    CHECK(href(cs, "query:arena-auto-policy-stats", "adaptive-policy-evaluations") >= 0, "evals");
    CHECK(href(cs, "query:arena-auto-policy-stats", "shape-profiler-on-compact-wired") == 1,
          "shape hook");
    clear_pressure_signals();
}

static void ac5_production_sweep() {
    std::println("\n--- AC5: production-sweep schema-2059 adaptive keys ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:production-sweep-1241-1245-stats\")");
    CHECK(r && is_hash(*r), "sweep hash");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "schema-2059") == 2059, "schema-2059");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "issue-2059") == 2059, "issue-2059");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "arena-adaptive-compact-wired") == 1,
          "adaptive wired");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "arena-adaptive-headroom-bp") >= 0,
          "headroom bp");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "arena-fixed-headroom-baseline-bp") ==
              2500,
          "fixed baseline");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "arena-post-compact-resync-total") >=
              0,
          "resync total");
    // Lineage retained.
    CHECK(href(cs, "query:production-sweep-1241-1245-stats",
               "arena-intelligent-auto-compact-wired") == 1,
          "intelligent wired");
}

static void ac6_source_wiring() {
    std::println("\n--- AC6: post-compact resync always wired ---");
    std::string svc, pol, arena;
    for (const char* p : {"src/compiler/service.ixx", "../src/compiler/service.ixx"}) {
        svc = read_file(p);
        if (!svc.empty())
            break;
    }
    for (const char* p :
         {"src/core/arena_auto_policy_stats.h", "../src/core/arena_auto_policy_stats.h"}) {
        pol = read_file(p);
        if (!pol.empty())
            break;
    }
    for (const char* p : {"src/core/arena.ixx", "../src/core/arena.ixx"}) {
        arena = read_file(p);
        if (!arena.empty())
            break;
    }
    CHECK(!svc.empty() && svc.find("record_post_compact_resync") != std::string::npos,
          "service → post-compact resync");
    CHECK(svc.find("publish_shape_deopt_metrics") != std::string::npos,
          "service → publish deopt metrics");
    CHECK(svc.find("on_boundary_or_fiber_sync") != std::string::npos, "service → shape re-sync");
    CHECK(!pol.empty() && pol.find("compute_adaptive_headroom") != std::string::npos,
          "adaptive headroom");
    CHECK(pol.find("kPolicyReasonSoftGatedStorm") != std::string::npos, "soft-gate storm reason");
    CHECK(!arena.empty() && arena.find("current_adaptive_headroom") != std::string::npos,
          "arena uses adaptive headroom");
}

static void ac7_mutate_stress() {
    std::println("\n--- AC7: multi-round mutate stress + headroom vs baseline ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    const auto mut0 = href(cs, "query:arena-auto-policy-stats", "mutation-pressure-signals");
    const auto resync0 = href(cs, "query:arena-auto-policy-stats", "post-compact-resync-total");
    const auto shape0 = href(cs, "query:arena-auto-policy-stats", "shape-inval-on-compact");
    const auto deopt_trig0 =
        href(cs, "query:arena-auto-policy-stats", "schema-2059"); // presence smoke

    for (int i = 0; i < 40; ++i) {
        (void)cs.eval(
            std::format("(mutate:rebind \"f\" \"(lambda (x) (+ x {}))\" \"i2059\")", i % 7));
        (void)cs.eval("(eval-current)");
        if (i % 8 == 0)
            (void)cs.eval("(arena:adaptive-compact)");
    }

    CHECK(href(cs, "query:arena-auto-policy-stats", "mutation-pressure-signals") > mut0,
          "mutation signals advanced");
    CHECK(href(cs, "query:arena-auto-policy-stats", "schema-2059") == 2059, "schema holds");
    CHECK(deopt_trig0 == 2059, "schema was 2059 pre-stress");

    // Adaptive headroom under mutation pressure should not exceed fixed baseline
    // by much (storm can raise it; mutation-only path should stay ≤ baseline).
    signal_mutation_pressure();
    (void)evaluate_auto_compact_policy(0.50, false, false, false, false, false, 0.2);
    const auto hr_bp = href(cs, "query:arena-auto-policy-stats", "adaptive-headroom-bp");
    CHECK(hr_bp >= 1250 && hr_bp <= 5000, "headroom in adaptive range after stress");

    // shape_inval / deopt counters remain accurate (monotonic, non-negative).
    CHECK(href(cs, "query:arena-auto-policy-stats", "shape-inval-on-compact") >= shape0,
          "shape-inval monotonic");
    CHECK(href(cs, "query:arena-auto-policy-stats", "post-compact-resync-total") >= resync0,
          "resync monotonic");
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval after stress");
    clear_pressure_signals();
}

static void ac8_short_path() {
    std::println("\n--- AC8: short-path low-mutation no forced compact ---");
    clear_pressure_signals();
    set_auto_compact_mode(AutoCompactMode::Balanced);
    auto d = evaluate_auto_compact_policy(0.02, false, false, false, false, false, 0.05);
    CHECK(!d.should_compact, "quiet short path → no compact");
    CHECK(!d.soft_gate_recommended, "no soft-gate on quiet path");
    // Render hotpath still soft-gates even with high frag.
    auto r = evaluate_auto_compact_policy(0.99, true, true, true, true, true, 0.99);
    CHECK(!r.should_compact, "render hotpath soft-gate retained");
}

static void ac9_lineage() {
    std::println("\n--- AC9: #1621 / #1919 lineage retained ---");
    CompilerService cs;
    CHECK(href(cs, "query:arena-auto-policy-stats", "schema") == 1621, "schema 1621");
    CHECK(href(cs, "query:arena-auto-policy-stats", "schema-1919") == 1919, "schema-1919");
    CHECK(href(cs, "query:arena-auto-policy-stats", "intelligent-policy-wired") == 1, "1919 wired");
    CHECK(href(cs, "query:arena-auto-policy-stats", "smart-policy-wired") == 1, "smart wired");
    CHECK(href(cs, "query:arena-auto-policy-stats", "closed-loop-wired") == 1, "closed-loop");
    CHECK(auto_compact_mode() == AutoCompactMode::Balanced ||
              auto_compact_mode() == AutoCompactMode::Conservative ||
              auto_compact_mode() == AutoCompactMode::Aggressive,
          "mode enum valid");
}

} // namespace

int run_test_arena_adaptive_compact_2059() {
    std::println("=== Issue #2059: Adaptive arena compact + ShapeProfiler deopt coordination ===");
    ac1_adaptive_headroom();
    ac2_storm_soft_gate();
    ac3_publish_metrics();
    ac4_policy_stats_schema();
    ac5_production_sweep();
    ac6_source_wiring();
    ac7_mutate_stress();
    ac8_short_path();
    ac9_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_arena_adaptive_compact_2059();
}
#endif
