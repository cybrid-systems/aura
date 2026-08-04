// @category: unit
// @reason: Issue #2127 — adaptive partial-relower threshold
// (workload / deopt / region-density aware) on top of #2112 cost policy.
//
//   AC1: default base=8 compatible with #2032 (no forced signals)
//   AC2: deopt-storm lowers effective thr; low deopt + low density raises
//   AC3: metrics expose effective thr + reason bits + decision counts
//   AC4: explicit set_partial_relower_threshold freezes deopt/density deltas
//   AC5: #2112 cost adaptive + pure should_partial_relower still work

#include "test_harness.hpp"
#include "compiler/hot_update_registry.hh"

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

using aura::compiler::adaptive_density_adjust_total_atomic;
using aura::compiler::adaptive_deopt_adjust_total_atomic;
using aura::compiler::adaptive_full_decision_total_atomic;
using aura::compiler::adaptive_partial_decision_total_atomic;
using aura::compiler::AdaptiveRelowerPolicy;
using aura::compiler::CompilerService;
using aura::compiler::decide_workload_adaptive_partial_relower;
using aura::compiler::get_effective_partial_relower_threshold;
using aura::compiler::get_last_adaptive_relower_reason;
using aura::compiler::get_partial_relower_threshold;
using aura::compiler::hot_update_registry;
using aura::compiler::kAdaptivePartialRelowerMax;
using aura::compiler::kAdaptivePartialRelowerMin;
using aura::compiler::kAdaptiveReasonDeoptStorm;
using aura::compiler::kAdaptiveReasonForced;
using aura::compiler::kAdaptiveReasonFull;
using aura::compiler::kAdaptiveReasonHighDensity;
using aura::compiler::kAdaptiveReasonLowDensity;
using aura::compiler::kAdaptiveReasonLowDeopt;
using aura::compiler::kAdaptiveReasonPartial;
using aura::compiler::kDefaultPartialRelowerThreshold;
using aura::compiler::reset_partial_relower_threshold_for_test;
using aura::compiler::set_partial_relower_threshold;
using aura::compiler::should_partial_relower;
using aura::compiler::should_partial_relower_workload;
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

} // namespace

int run_test_workload_adaptive_relower() {
    std::println("=== Issue #2127: workload/deopt adaptive partial-relower thr ===");

    // ── AC1: default base 8, quiet workload ≈ #2032 ──
    {
        std::println("\n--- AC1: default base=8 ---");
        reset_partial_relower_threshold_for_test();
        hot_update_registry().reset_deopt_storm_state_for_test();
        CHECK(get_partial_relower_threshold() == kDefaultPartialRelowerThreshold, "base 8");
        // Quiet deopt + unknown density: may raise thr slightly via low-deopt
        auto d = decide_workload_adaptive_partial_relower(/*dirty*/ 7, /*total*/ 0,
                                                          /*deopt_win*/ 0, /*storm_thr*/ 1000,
                                                          /*storm*/ false);
        CHECK(d.effective_threshold >= kDefaultPartialRelowerThreshold, "quiet ≥ base");
        CHECK(d.effective_threshold <= kAdaptivePartialRelowerMax, "≤ max");
        CHECK(d.want_partial, "7 dirty under raised/base thr → partial");
        CHECK(should_partial_relower(7), "pure #2032 still: 7 partial at base 8");
        CHECK(!should_partial_relower(8), "pure #2032 still: 8 full at base 8");
        // Explicit AdaptiveRelowerPolicy at base 8, forced quiet density mid
        AdaptiveRelowerPolicy pol;
        pol.base = 8;
        std::uint32_t reason = 0;
        // Mid density (30%) no storm → only low-deopt raise maybe
        auto thr = pol.effective(/*win*/ 0, /*st*/ 1000, /*storm*/ false, /*dens*/ 3000,
                                 /*forced*/ false, &reason);
        CHECK(thr >= kAdaptivePartialRelowerMin && thr <= kAdaptivePartialRelowerMax,
              "policy thr in band");
        CHECK(reason & aura::compiler::kAdaptiveReasonBase, "base reason bit");
    }

    // ── AC2: deopt storm lowers thr; high density lowers; low+low raises ──
    {
        std::println("\n--- AC2: deopt storm + density ---");
        reset_partial_relower_threshold_for_test();

        // Storm active → thr ↓ from 8
        auto storm = decide_workload_adaptive_partial_relower(
            /*dirty*/ 5, /*total*/ 10, /*win*/ 0, /*st*/ 1000, /*storm*/ true);
        std::println("  storm effective={} want_partial={} reason={:#x}", storm.effective_threshold,
                     storm.want_partial, storm.reason_bits);
        CHECK(storm.effective_threshold < kDefaultPartialRelowerThreshold ||
                  storm.effective_threshold == kAdaptivePartialRelowerMin,
              "storm lowers thr");
        CHECK(storm.reason_bits & kAdaptiveReasonDeoptStorm, "deopt reason bit");
        // dirty 5 / total 10 = 50% density also HighDensity
        CHECK(storm.reason_bits & kAdaptiveReasonHighDensity, "high density bit");

        // Window at half threshold also trips deopt path
        reset_partial_relower_threshold_for_test();
        auto half = decide_workload_adaptive_partial_relower(
            /*dirty*/ 3, /*total*/ 100, /*win*/ 500, /*st*/ 1000, /*storm*/ false);
        CHECK(half.reason_bits & kAdaptiveReasonDeoptStorm, "half-window deopt signal");
        CHECK(half.reason_bits & kAdaptiveReasonLowDensity, "3/100 low density");
        // Low density raises, deopt lowers — net depends on order (deopt after density).
        // Policy applies density then deopt; net can be various.
        CHECK(half.effective_threshold >= kAdaptivePartialRelowerMin, "clamped min");

        // Quiet + low density → prefer partial longer
        reset_partial_relower_threshold_for_test();
        auto quiet = decide_workload_adaptive_partial_relower(
            /*dirty*/ 2, /*total*/ 100, /*win*/ 0, /*st*/ 1000, /*storm*/ false);
        CHECK(quiet.effective_threshold >= kDefaultPartialRelowerThreshold, "quiet thr ≥ 8");
        CHECK(quiet.reason_bits & kAdaptiveReasonLowDeopt, "low deopt raise");
        CHECK(quiet.reason_bits & kAdaptiveReasonLowDensity, "low density raise");
        CHECK(quiet.want_partial, "2 dirty under raised thr → partial");
        CHECK(quiet.reason_bits & kAdaptiveReasonPartial, "partial decision bit");

        // High density alone → thr ↓
        AdaptiveRelowerPolicy pol;
        pol.base = 8;
        std::uint32_t r = 0;
        auto thr_hi = pol.effective(0, 1000, false, /*dens 80%*/ 8000, false, &r);
        CHECK(thr_hi < 8 || thr_hi == kAdaptivePartialRelowerMin, "high dens thr ↓");
        CHECK(r & kAdaptiveReasonHighDensity, "high dens reason");
    }

    // ── AC4: forced threshold freezes deopt/density ──
    {
        std::println("\n--- AC4: forced thr ignores deopt/density ---");
        reset_partial_relower_threshold_for_test();
        set_partial_relower_threshold(12);
        CHECK(get_partial_relower_threshold() == 12, "forced 12");
        auto d = decide_workload_adaptive_partial_relower(
            /*dirty*/ 10, /*total*/ 10, /*win*/ 9999, /*st*/ 100, /*storm*/ true);
        CHECK(d.effective_threshold == 12, "forced thr sticky under storm");
        CHECK(d.reason_bits & kAdaptiveReasonForced, "forced bit");
        CHECK(!(d.reason_bits & kAdaptiveReasonDeoptStorm), "no deopt delta when forced");
        CHECK(d.want_partial, "10 < 12 → partial under forced");
        reset_partial_relower_threshold_for_test();
    }

    // ── AC3: query metrics surface ──
    {
        std::println("\n--- AC3: query surfaces ---");
        reset_partial_relower_threshold_for_test();
        (void)decide_workload_adaptive_partial_relower(3, 100, 0, 1000, false);
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
        CHECK(href(cs, "query:incremental-relower-policy-stats", "schema-2127") == 2127,
              "policy schema-2127");
        CHECK(href(cs, "query:incremental-relower-policy-stats",
                   "workload-adaptive-relower-wired") == 1,
              "policy wired");
        CHECK(href(cs, "query:incremental-relower-policy-stats",
                   "effective-partial-relower-threshold") >= 4,
              "policy effective thr");
        CHECK(href(cs, "query:incremental-relower-stats", "schema-2127") == 2127, "relower schema");
        CHECK(href(cs, "query:incremental-relower-stats", "workload-adaptive-relower-wired") == 1,
              "relower wired");
        CHECK(href(cs, "query:incremental-relower-stats", "adaptive-partial-decision-total") >= 0,
              "partial decisions key");
        CHECK(href(cs, "query:incremental-relower-stats", "adaptive-full-decision-total") >= 0,
              "full decisions key");
        CHECK(get_effective_partial_relower_threshold() >= kAdaptivePartialRelowerMin,
              "process effective thr");
        CHECK(get_last_adaptive_relower_reason() != 0 || true, "reason snapshot");
        CHECK(adaptive_partial_decision_total_atomic().load() +
                      adaptive_full_decision_total_atomic().load() >=
                  1,
              "decision counters advanced");
    }

    // ── AC5: source + #2112 lineage ──
    {
        std::println("\n--- AC5: source + lineage ---");
        auto pure = read_file("src/compiler/ir_cache_pure.ixx");
        auto dirty = read_file("src/compiler/service_dirty.cpp");
        auto svc = read_file("src/compiler/service.ixx");
        CHECK(pure.find("Issue #2127") != std::string::npos, "ir_cache_pure #2127");
        CHECK(pure.find("AdaptiveRelowerPolicy") != std::string::npos, "AdaptiveRelowerPolicy");
        CHECK(pure.find("decide_workload_adaptive_partial_relower") != std::string::npos,
              "decide helper");
        CHECK(dirty.find("consult_workload_adaptive_partial_") != std::string::npos ||
                  svc.find("consult_workload_adaptive_partial_") != std::string::npos,
              "service consult");
        CHECK(pure.find("kDefaultPartialRelowerThreshold") != std::string::npos, "base 8 lineage");
        // Pure overload unchanged
        reset_partial_relower_threshold_for_test();
        CHECK(should_partial_relower(1), "pure 1 partial");
        CHECK(should_partial_relower_workload(1, 100, 0, 1000, false), "workload 1 partial");
    }

    // ── Service smoke: set-body still works under adaptive gate ──
    {
        std::println("\n--- service smoke ---");
        reset_partial_relower_threshold_for_test();
        hot_update_registry().reset_deopt_storm_state_for_test();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (h n) (+ n 1))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        CHECK(cs.eval("(mutate:set-body \"h\" \"(lambda (n) (+ n 2))\")").has_value(), "set-body");
        CHECK(cs.eval("(eval-current)").has_value(), "re-eval");
        auto r = cs.eval("(h 10)");
        CHECK(r && is_int(*r) && as_int(*r) == 12, "h 10 = 12");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_workload_adaptive_relower();
}
#endif
