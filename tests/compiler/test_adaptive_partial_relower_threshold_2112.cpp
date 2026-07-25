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

} // namespace

int main() {
    std::println("=== Issue #2112: adaptive partial relower threshold ===");
    ac1_cold_start();
    ac2_adaptive_moves();
    ac3_query_surface();
    ac4_force_threshold_hook();
    ac5_regression_pure();
    reset_partial_relower_threshold_for_test();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
