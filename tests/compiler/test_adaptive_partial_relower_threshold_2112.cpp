// @category: unit
// @reason: Issue #2112 — adaptive partial/full relower threshold
// (replace hard-coded ≥8).
//
//   AC1: Cold-start stays at default 8 until enough samples
//   AC2: After samples, threshold moves within [4, 32] by cost ratio
//   AC3: query surfaces threshold + samples + win ratio
//   AC4: test hook forces threshold; decision changes at boundary
//   AC5: no regression on pure overloads / default suites

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.ir_cache_pure;
import aura.compiler.value;

namespace {

using aura::compiler::avg_full_relower_cost_ns;
using aura::compiler::avg_partial_relower_cost_ns;
using aura::compiler::CompilerService;
using aura::compiler::estimate_relower_blocks;
using aura::compiler::full_relower_cost_samples_atomic;
using aura::compiler::get_partial_relower_threshold;
using aura::compiler::inject_adaptive_relower_cost_samples_for_test;
using aura::compiler::kAdaptivePartialRelowerMax;
using aura::compiler::kAdaptivePartialRelowerMin;
using aura::compiler::kAdaptiveRelowerMinSamples;
using aura::compiler::kDefaultPartialRelowerThreshold;
using aura::compiler::partial_relower_cost_samples_atomic;
using aura::compiler::partial_relower_threshold_is_forced;
using aura::compiler::partial_vs_full_win_ratio_bp;
using aura::compiler::reset_partial_relower_threshold_for_test;
using aura::compiler::set_partial_relower_threshold;
using aura::compiler::should_partial_relower;
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

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_cold_start() {
    std::println("\n--- AC1: cold-start stays at default 8 ---");
    reset_partial_relower_threshold_for_test();
    CHECK(get_partial_relower_threshold() == kDefaultPartialRelowerThreshold, "default 8");
    CHECK(!partial_relower_threshold_is_forced(), "not forced");
    CHECK(partial_relower_cost_samples_atomic().load() == 0, "no partial samples");
    CHECK(full_relower_cost_samples_atomic().load() == 0, "no full samples");
    // Few samples must not move threshold off 8
    inject_adaptive_relower_cost_samples_for_test(/*partial*/ 100, /*full*/ 10000,
                                                  /*n*/ kAdaptiveRelowerMinSamples - 1);
    CHECK(get_partial_relower_threshold() == kDefaultPartialRelowerThreshold,
          "still 8 before min samples on both sides");
    CHECK(should_partial_relower(7), "7 → partial at 8");
    CHECK(!should_partial_relower(8), "8 → full at 8");
}

static void ac2_adaptive_moves() {
    std::println("\n--- AC2: threshold adapts within [4,32] ---");
    reset_partial_relower_threshold_for_test();
    // Full much more expensive → raise threshold (prefer partial longer)
    inject_adaptive_relower_cost_samples_for_test(/*partial*/ 100, /*full*/ 10000,
                                                  /*n*/ kAdaptiveRelowerMinSamples + 4);
    const auto thr_hi = get_partial_relower_threshold();
    CHECK(thr_hi >= kDefaultPartialRelowerThreshold, "thr raised or stayed ≥8");
    CHECK(thr_hi <= kAdaptivePartialRelowerMax, "thr ≤ 32");
    CHECK(avg_partial_relower_cost_ns() > 0, "avg partial");
    CHECK(avg_full_relower_cost_ns() > 0, "avg full");
    CHECK(partial_vs_full_win_ratio_bp() > 10000, "full costlier → ratio > 10000");

    // Force full cheaper path: reset then inject full cheaper
    reset_partial_relower_threshold_for_test();
    // Start thr at 8; full cheaper relative → thr may drop
    inject_adaptive_relower_cost_samples_for_test(/*partial*/ 5000, /*full*/ 1000,
                                                  /*n*/ kAdaptiveRelowerMinSamples + 8);
    const auto thr_lo = get_partial_relower_threshold();
    CHECK(thr_lo >= kAdaptivePartialRelowerMin, "thr ≥ 4");
    CHECK(thr_lo <= kAdaptivePartialRelowerMax, "thr ≤ 32");
    // Decision boundary moves with thr
    CHECK(should_partial_relower(thr_lo - 1), "below thr → partial");
    CHECK(!should_partial_relower(thr_lo), "at thr → full");
}

static void ac3_query_surface() {
    std::println("\n--- AC3: query surfaces ---");
    reset_partial_relower_threshold_for_test();
    inject_adaptive_relower_cost_samples_for_test(200, 2000, kAdaptiveRelowerMinSamples);
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
    // Dedicated policy query
    CHECK(href(cs, "query:incremental-relower-policy-stats", "schema-2112") == 2112,
          "policy schema-2112");
    CHECK(href(cs, "query:incremental-relower-policy-stats", "adaptive-partial-relower-wired") == 1,
          "policy wired");
    CHECK(href(cs, "query:incremental-relower-policy-stats", "partial-relower-threshold") >= 4,
          "policy thr");
    CHECK(href(cs, "query:incremental-relower-policy-stats", "partial-relower-cost-samples") >=
              static_cast<std::int64_t>(kAdaptiveRelowerMinSamples),
          "policy samples");
    CHECK(href(cs, "query:incremental-relower-policy-stats", "partial-vs-full-win-ratio-bp") >= 0,
          "policy win ratio");
    // Also on incremental-relower-stats
    CHECK(href(cs, "query:incremental-relower-stats", "schema-2112") == 2112, "stats schema-2112");
    CHECK(href(cs, "query:incremental-relower-stats", "adaptive-partial-relower-wired") == 1,
          "stats wired");
}

static void ac4_force_threshold_hook() {
    std::println("\n--- AC4: force threshold via test hook ---");
    reset_partial_relower_threshold_for_test();
    set_partial_relower_threshold(4);
    CHECK(partial_relower_threshold_is_forced(), "forced");
    CHECK(get_partial_relower_threshold() == 4, "thr=4");
    CHECK(should_partial_relower(3), "3 partial at 4");
    CHECK(!should_partial_relower(4), "4 full at 4");
    // Adaptive must not override forced
    inject_adaptive_relower_cost_samples_for_test(10, 100000, kAdaptiveRelowerMinSamples + 5);
    CHECK(get_partial_relower_threshold() == 4, "forced thr sticky under samples");
    set_partial_relower_threshold(16);
    CHECK(should_partial_relower(8), "8 partial at 16");
    CHECK(!should_partial_relower(16), "16 full at 16");
    reset_partial_relower_threshold_for_test();
    CHECK(get_partial_relower_threshold() == 8, "reset default");
    CHECK(!partial_relower_threshold_is_forced(), "unforced");
}

static void ac5_regression_pure() {
    std::println("\n--- AC5: pure overloads + source wiring ---");
    CHECK(should_partial_relower(3, 4), "pure thr4");
    CHECK(!should_partial_relower(4, 4), "pure thr4 full");
    CHECK(estimate_relower_blocks(5, 8) == 5, "est 5");
    CHECK(estimate_relower_blocks(8, 8) == static_cast<std::size_t>(-1), "est full");
    auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    auto svc = read_file("src/compiler/service.ixx");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(pure.find("Issue #2112") != std::string::npos || pure.find("#2112") != std::string::npos,
          "pure cites #2112");
    CHECK(pure.find("note_partial_relower_cost_ns") != std::string::npos, "note partial");
    CHECK(pure.find("maybe_adapt_partial_relower_threshold") != std::string::npos, "adapt");
    CHECK(svc.find("note_partial_relower_cost_ns") != std::string::npos, "service partial sample");
    CHECK(svc.find("note_full_relower_cost_ns") != std::string::npos, "service full sample");
    CHECK(q.find("query:incremental-relower-policy-stats") != std::string::npos, "policy query");
    CHECK(q.find("schema-2112") != std::string::npos, "schema-2112");
}


// AC (Issue #2248): Agent-driven adaptive relower threshold from
// fallback-reason telemetry. AC1 (bad reasons raise thr), AC2
// (clean window decays), AC3 (env override freezes).
void ac2248_agent_driven_adaptive_thr() {
    std::println("\n--- AC #2248: Agent-driven adaptive thr ---");
    auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    auto met = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    // Policy + helpers in ir_cache_pure.ixx
    CHECK(pure.find("AdaptiveThrPolicy") != std::string::npos, "AdaptiveThrPolicy struct");
    CHECK(pure.find("current_adaptive_partial_thr") != std::string::npos,
          "current_adaptive_partial_thr getter");
    CHECK(pure.find("note_relower_fallback_for_adaptive") != std::string::npos,
          "note_relower_fallback_for_adaptive helper");
    CHECK(pure.find("adaptive_thr_frozen") != std::string::npos,
          "adaptive_thr_frozen env override");
    CHECK(pure.find("AURA_ADAPTIVE_THR") != std::string::npos, "env override AURA_ADAPTIVE_THR");
    CHECK(pure.find("MapInconsistent") != std::string::npos &&
              pure.find("DesyncForceFull") != std::string::npos,
          "correctness-risk reasons referenced");
    // 5 atomic counters in observability_metrics.h
    CHECK(met.find("adaptive_thr_current{800}") != std::string::npos, "adaptive_thr_current field");
    CHECK(met.find("adaptive_thr_raises_total{0}") != std::string::npos, "raises counter field");
    CHECK(met.find("adaptive_thr_decays_total{0}") != std::string::npos, "decays counter field");
    CHECK(met.find("adaptive_thr_bad_window_count{0}") != std::string::npos,
          "bad window count field");
    CHECK(met.find("adaptive_thr_frozen{0}") != std::string::npos, "frozen field");
    CHECK(met.find("note_relower_fallback_for_adaptive") != std::string::npos,
          "note_relower_fallback feeds adaptive policy");
    // Query surface (4 new keys + schema-2248)
    CHECK(q.find("adaptive-thr-current") != std::string::npos, "adaptive-thr-current key");
    CHECK(q.find("adaptive-thr-raises-total") != std::string::npos,
          "adaptive-thr-raises-total key");
    CHECK(q.find("adaptive-thr-decays-total") != std::string::npos,
          "adaptive-thr-decays-total key");
    CHECK(q.find("adaptive-thr-bad-window-count") != std::string::npos,
          "adaptive-thr-bad-window-count key");
    CHECK(q.find("adaptive-thr-frozen") != std::string::npos, "adaptive-thr-frozen key");
    CHECK(q.find("adaptive-thr-wired") != std::string::npos, "wired sentinel");
    CHECK(q.find("schema-2248") != std::string::npos, "schema-2248 lineage");
    CHECK(q.find("issue-2248") != std::string::npos, "issue-2248 lineage");
    // Runtime smoke (AC1): reset, inject 20 MapInconsistent, thr should rise.
    aura::compiler::reset_adaptive_thr_for_test();
    const auto base_thr = aura::compiler::current_adaptive_partial_thr();
    aura::compiler::inject_adaptive_thr_bad_for_test(20);
    const auto raised_thr = aura::compiler::current_adaptive_partial_thr();
    CHECK(raised_thr > base_thr, "AC1: raised_thr > base after 20 bad events");
    CHECK(raised_thr <= (base_thr * 25) / 10, "AC1: raised_thr <= 2.5x base (cap)");
    // AC2: clean window decay.
    const auto peak_thr = raised_thr;
    for (int i = 0; i < 30; ++i) {
        // Ok=0 per RelowerFallbackReason ABI (avoid observability_metrics.h dep).
        aura::compiler::note_relower_fallback_for_adaptive(/*Ok*/ 0);
    }
    const auto decayed_thr = aura::compiler::current_adaptive_partial_thr();
    CHECK(decayed_thr < peak_thr, "AC2: decayed_thr < peak after clean window");
    CHECK(decayed_thr >= base_thr, "AC2: decayed_thr >= base (no ratchet below base)");
    // Restore for downstream tests.
    aura::compiler::reset_adaptive_thr_for_test();
}

} // namespace

int run_test_adaptive_partial_relower_threshold_2112() {
    std::println("=== Issue #2112: adaptive partial relower threshold ===");
    ac1_cold_start();
    ac2_adaptive_moves();
    ac3_query_surface();
    ac4_force_threshold_hook();
    ac5_regression_pure();
    ac2248_agent_driven_adaptive_thr();
    reset_partial_relower_threshold_for_test();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_adaptive_partial_relower_threshold_2112();
}
#endif
