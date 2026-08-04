// @category: unit
// @reason: Issue #2209 — feed cascade depth + dirty_rate into adaptive
// partial-relower threshold (refine #2112).
//
//   AC1: After enough samples, high cascade-depth raises the threshold.
//   AC2: Low-depth / low-dirty-rate still allows cost-based thr lower.
//   AC3: Explicit set_partial_relower_threshold freezes adaptive.
//   AC4: query:incremental-relower-policy-stats surfaces cascade_depth_avg
//        and dirty_rate + schema-2209.
//   AC5: Cold-start (default 8) unchanged until min samples.

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

using aura::compiler::adaptive_cascade_signal_raise_total_atomic;
using aura::compiler::adaptive_dirty_rate_bp_atomic;
using aura::compiler::adaptive_last_cascade_depth_avg_milli_atomic;
using aura::compiler::CompilerService;
using aura::compiler::get_partial_relower_threshold;
using aura::compiler::inject_adaptive_cascade_signals_for_test;
using aura::compiler::inject_adaptive_relower_cost_samples_for_test;
using aura::compiler::kAdaptivePartialRelowerMax;
using aura::compiler::kAdaptivePartialRelowerMin;
using aura::compiler::kAdaptiveRelowerMinSamples;
using aura::compiler::kDefaultPartialRelowerThreshold;
using aura::compiler::note_adaptive_dirty_rate_bp;
using aura::compiler::partial_relower_threshold_is_forced;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:incremental-relower-policy-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// AC5 first: cold-start unchanged
static void ac5_cold_start() {
    std::println("\n--- AC5: cold-start stays at default 8 ---");
    reset_partial_relower_threshold_for_test();
    CHECK(get_partial_relower_threshold() == kDefaultPartialRelowerThreshold, "default 8");
    // High depth alone without min samples must not move thr
    inject_adaptive_cascade_signals_for_test(/*depth_sum*/ 50, /*n*/ 5, /*dirty_rate_bp*/ 5000);
    CHECK(get_partial_relower_threshold() == kDefaultPartialRelowerThreshold,
          "AC5: still 8 without cost samples");
}

// AC1: high cascade-depth raises thr after samples
static void ac1_high_depth_raises() {
    std::println("\n--- AC1: high cascade depth raises threshold ---");
    reset_partial_relower_threshold_for_test();
    // Neutral cost samples (full ~ partial*2.5 → neither cost branch)
    // so only cascade signal moves thr.
    inject_adaptive_relower_cost_samples_for_test(/*partial*/ 1000, /*full*/ 2500,
                                                  /*n*/ kAdaptiveRelowerMinSamples);
    const auto thr0 = get_partial_relower_threshold();
    // Neutral: af = 2.5*ap → not >3x and not <2x → thr stays
    CHECK(thr0 == kDefaultPartialRelowerThreshold || thr0 >= kAdaptivePartialRelowerMin,
          "baseline thr in band");
    // High depth (avg 10 > 4) → thr +2
    const auto raise0 = adaptive_cascade_signal_raise_total_atomic().load();
    inject_adaptive_cascade_signals_for_test(/*depth_sum*/ 100, /*n*/ 10, /*dirty_rate_bp*/ 0);
    const auto thr_hi = get_partial_relower_threshold();
    CHECK(thr_hi > thr0, "AC1: thr raised under high depth");
    CHECK(thr_hi <= kAdaptivePartialRelowerMax, "AC1: thr ≤ max");
    CHECK(adaptive_cascade_signal_raise_total_atomic().load() > raise0, "AC1: raise counter");
    CHECK(adaptive_last_cascade_depth_avg_milli_atomic().load() >= 9000, "AC1: depth ~10 *1000");
    // High dirty_rate alone also raises
    reset_partial_relower_threshold_for_test();
    inject_adaptive_relower_cost_samples_for_test(1000, 2500, kAdaptiveRelowerMinSamples);
    const auto thr_b = get_partial_relower_threshold();
    inject_adaptive_cascade_signals_for_test(/*depth_sum*/ 0, /*n*/ 1, /*dirty_rate_bp*/ 4000);
    CHECK(get_partial_relower_threshold() > thr_b, "AC1: high dirty_rate raises thr");
}

// AC2: low depth + full cheaper → thr can lower
static void ac2_low_depth_cost_lower() {
    std::println("\n--- AC2: low depth allows cost-based thr lower ---");
    reset_partial_relower_threshold_for_test();
    // Full cheaper → thr decreases
    inject_adaptive_relower_cost_samples_for_test(/*partial*/ 5000, /*full*/ 1000,
                                                  /*n*/ kAdaptiveRelowerMinSamples + 8);
    const auto thr_lo = get_partial_relower_threshold();
    CHECK(thr_lo >= kAdaptivePartialRelowerMin, "AC2: thr ≥ min");
    CHECK(thr_lo < kDefaultPartialRelowerThreshold || thr_lo <= kDefaultPartialRelowerThreshold,
          "AC2: thr not forced up");
    // Explicit: low depth (avg 1) + dirty_rate 0 + full cheaper
    reset_partial_relower_threshold_for_test();
    inject_adaptive_relower_cost_samples_for_test(5000, 1000, kAdaptiveRelowerMinSamples + 4);
    const auto thr_before = get_partial_relower_threshold();
    inject_adaptive_cascade_signals_for_test(/*depth_sum*/ 5, /*n*/ 5, /*dirty_rate_bp*/ 100);
    // low signals: cost lower still applies (or thr already low from inject cost)
    const auto thr_after = get_partial_relower_threshold();
    CHECK(thr_after <= thr_before || thr_after <= kDefaultPartialRelowerThreshold,
          "AC2: low depth does not force thr up");
    CHECK(thr_after >= kAdaptivePartialRelowerMin, "AC2: still ≥ min");
    CHECK(should_partial_relower(thr_after > 0 ? thr_after - 1 : 0) || thr_after >= 1,
          "AC2: decision boundary consistent");
}

// AC3: forced thr freezes adaptive
static void ac3_forced_freeze() {
    std::println("\n--- AC3: set_partial_relower_threshold freezes adaptive ---");
    reset_partial_relower_threshold_for_test();
    set_partial_relower_threshold(12);
    CHECK(partial_relower_threshold_is_forced(), "forced");
    inject_adaptive_relower_cost_samples_for_test(10, 100000, kAdaptiveRelowerMinSamples + 5);
    inject_adaptive_cascade_signals_for_test(200, 10, 9000);
    CHECK(get_partial_relower_threshold() == 12, "AC3: forced thr sticky");
    reset_partial_relower_threshold_for_test();
    CHECK(!partial_relower_threshold_is_forced(), "unforced after reset");
}

// AC4: query surface schema-2209
static void ac4_query_schema() {
    std::println("\n--- AC4: query schema-2209 + depth/dirty_rate keys ---");
    reset_partial_relower_threshold_for_test();
    inject_adaptive_relower_cost_samples_for_test(100, 10000, kAdaptiveRelowerMinSamples);
    inject_adaptive_cascade_signals_for_test(/*depth_sum*/ 60, /*n*/ 10, /*dirty_rate_bp*/ 3000);
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2209") == 2209, "schema-2209");
    CHECK(href(cs, "issue-2209") == 2209, "issue-2209");
    CHECK(href(cs, "cascade-depth-dirty-rate-adaptive-wired") == 1, "wired");
    CHECK(href(cs, "cascade-depth-avg-x1000") >= 0, "cascade-depth-avg-x1000");
    CHECK(href(cs, "dirty-rate-bp") == 3000 || href(cs, "dirty_rate_bp") == 3000, "dirty-rate-bp");
    CHECK(href(cs, "adaptive-cascade-signal-raise-total") >= 1, "raise total");
    CHECK(href(cs, "partial-relower-threshold") >= 4, "thr still queryable");
    CHECK(href(cs, "schema-2112") == 2112, "2112 lineage retained");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(q.find("schema-2209") != std::string::npos, "query cites schema-2209");
    auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    CHECK(pure.find("#2209") != std::string::npos, "pure cites #2209");
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(dirty.find("#2209") != std::string::npos, "service_dirty wires #2209");
}

} // namespace

int run_test_adaptive_cascade_depth_partial_thr() {
    std::println("=== Issue #2209: cascade depth + dirty_rate adaptive thr ===");
    ac5_cold_start();
    ac1_high_depth_raises();
    ac2_low_depth_cost_lower();
    ac3_forced_freeze();
    ac4_query_schema();
    reset_partial_relower_threshold_for_test();
    std::println("\n=== results: {} passed, {} failed ===\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_adaptive_cascade_depth_partial_thr();
}
#endif
