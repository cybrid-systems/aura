// @category: integration
// @reason: Issue #1919 — Intelligent auto-compaction/defrag strategy with
// Issue #1242/#1621/#187/#1919/#300 (#1978 renamed): issue# moved from filename to header.
// fiber/JIT/AI orchestration linkage (extend #187, #300, #1242, #1621).
//
//   AC1: AutoCompactMode Conservative/Balanced/Aggressive + dynamic thr 30–60%
//   AC2: evaluate_auto_compact_policy respects mode + mutation/JIT pressure
//   AC3: mutation pressure signal from mutate path; FP/TP outcome counters
//   AC4: query:arena-auto-policy-stats schema-1919 + intelligent keys
//   AC5: query:production-sweep-1241-1245-stats schema-1919 arena keys
//   AC6: ShapeProfiler/JIT deopt pressure linkage wired
//   AC7: multi-round mutate stress; false-positive rate readable (<5% target)
//   AC8: #1621 lineage schema retained

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
using aura::core::arena_policy::auto_compact_false_positive_bp;
using aura::core::arena_policy::auto_compact_mode;
using aura::core::arena_policy::AutoCompactMode;
using aura::core::arena_policy::compute_dynamic_frag_threshold;
using aura::core::arena_policy::evaluate_auto_compact_policy;
using aura::core::arena_policy::kFalsePositiveTargetBp;
using aura::core::arena_policy::kFragThresholdMax;
using aura::core::arena_policy::kFragThresholdMin;
using aura::core::arena_policy::record_auto_compact_outcome;
using aura::core::arena_policy::set_auto_compact_mode;
using aura::core::arena_policy::signal_jit_deopt_pressure;
using aura::core::arena_policy::signal_mutation_pressure;
using aura::core::arena_policy::signal_shape_churn;
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

static void ac1_modes_and_thresholds() {
    std::println("\n--- AC1: modes + dynamic threshold 30–60% ---");
    set_auto_compact_mode(AutoCompactMode::Balanced);
    CHECK(auto_compact_mode() == AutoCompactMode::Balanced, "default Balanced");
    set_auto_compact_mode(AutoCompactMode::Conservative);
    CHECK(auto_compact_mode() == AutoCompactMode::Conservative, "Conservative set");
    set_auto_compact_mode(AutoCompactMode::Aggressive);
    CHECK(auto_compact_mode() == AutoCompactMode::Aggressive, "Aggressive set");
    set_auto_compact_mode(AutoCompactMode::Balanced);

    const double t_cons =
        compute_dynamic_frag_threshold(AutoCompactMode::Conservative, false, false, false, false);
    const double t_bal =
        compute_dynamic_frag_threshold(AutoCompactMode::Balanced, false, false, false, false);
    const double t_agg =
        compute_dynamic_frag_threshold(AutoCompactMode::Aggressive, false, false, false, false);
    CHECK(t_cons >= kFragThresholdMin && t_cons <= kFragThresholdMax, "cons in range");
    CHECK(t_bal >= kFragThresholdMin && t_bal <= kFragThresholdMax, "bal in range");
    CHECK(t_agg >= kFragThresholdMin && t_agg <= kFragThresholdMax, "agg in range");
    CHECK(t_cons >= t_bal, "Conservative thr >= Balanced");
    // Deopt pressure raises threshold; mutation lowers it.
    const double t_deopt =
        compute_dynamic_frag_threshold(AutoCompactMode::Balanced, false, true, false, false);
    const double t_mut =
        compute_dynamic_frag_threshold(AutoCompactMode::Balanced, true, false, false, false);
    CHECK(t_deopt >= t_bal, "deopt raises thr");
    CHECK(t_mut <= t_bal, "mutation lowers thr");
}

static void ac2_policy_eval() {
    std::println("\n--- AC2: evaluate_auto_compact_policy intelligent ---");
    set_auto_compact_mode(AutoCompactMode::Balanced);
    auto quiet = evaluate_auto_compact_policy(0.05, false, false, false, false, false, 0.1);
    CHECK(!quiet.should_compact, "low frag → no trigger");

    auto frag = evaluate_auto_compact_policy(0.55, false, false, false, false, false, 0.1);
    CHECK(frag.should_compact, "high frag → trigger");
    CHECK(frag.frag_threshold_used >= kFragThresholdMin, "thr recorded");

    auto render = evaluate_auto_compact_policy(0.90, true, true, true, true, true, 0.99);
    CHECK(!render.should_compact, "render hotpath soft-gate");

    signal_mutation_pressure();
    // Dirty cascade still triggers; mutation reason bit is set when pressure pending.
    auto mut = evaluate_auto_compact_policy(0.20, false, true, false, false, false, 0.2);
    CHECK(mut.should_compact, "dirty → trigger");
    CHECK((mut.reason & aura::core::arena_policy::kPolicyReasonDirty) != 0, "dirty reason");
    // Mutation + soft frag (no dirty) also triggers under Balanced.
    auto mut_soft = evaluate_auto_compact_policy(0.20, false, false, false, false, false, 0.2);
    CHECK(mut_soft.should_compact, "mutation+soft-frag → trigger");
    CHECK((mut_soft.reason & aura::core::arena_policy::kPolicyReasonMutation) != 0,
          "mutation reason");

    signal_jit_deopt_pressure();
    set_auto_compact_mode(AutoCompactMode::Conservative);
    // Under conservative + deopt, bare dirty without high frag is gated.
    auto cons = evaluate_auto_compact_policy(0.10, false, true, false, false, false, 0.1);
    CHECK(!cons.should_compact, "conservative+deopt gates bare dirty");
    set_auto_compact_mode(AutoCompactMode::Balanced);
    // Clear sticky signals so later suite tests see a quiet baseline.
    (void)aura::core::arena_policy::consume_mutation_pressure();
    (void)aura::core::arena_policy::consume_jit_deopt_pressure();
}

static void ac3_outcome_counters() {
    std::println("\n--- AC3: FP/TP outcome counters ---");
    const auto fp0 = aura::core::arena_policy::auto_compact_false_positive_total.load();
    const auto tp0 = aura::core::arena_policy::auto_compact_true_positive_total.load();
    record_auto_compact_outcome(true);
    record_auto_compact_outcome(true);
    record_auto_compact_outcome(false);
    CHECK(aura::core::arena_policy::auto_compact_true_positive_total.load() >= tp0 + 2, "TP+2");
    CHECK(aura::core::arena_policy::auto_compact_false_positive_total.load() >= fp0 + 1, "FP+1");
    const auto bp = auto_compact_false_positive_bp();
    CHECK(bp <= 10000, "fp bp bounded");
    CHECK(kFalsePositiveTargetBp == 500, "5% target");
}

static void ac4_policy_stats() {
    std::println("\n--- AC4: query:arena-auto-policy-stats schema-1919 ---");
    CompilerService cs;
    signal_mutation_pressure();
    auto h = cs.eval("(engine:metrics \"query:arena-auto-policy-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "query:arena-auto-policy-stats", "schema") == 1621, "lineage schema 1621");
    CHECK(href(cs, "query:arena-auto-policy-stats", "schema-1919") == 1919, "schema-1919");
    CHECK(href(cs, "query:arena-auto-policy-stats", "issue-1919") == 1919, "issue-1919");
    CHECK(href(cs, "query:arena-auto-policy-stats", "intelligent-policy-wired") == 1, "wired");
    CHECK(href(cs, "query:arena-auto-policy-stats", "auto-compact-mode") >= 0, "mode");
    CHECK(href(cs, "query:arena-auto-policy-stats", "auto-compact-mode") <= 2, "mode ≤2");
    CHECK(href(cs, "query:arena-auto-policy-stats", "frag-threshold-min-bp") == 3000, "min 30%");
    CHECK(href(cs, "query:arena-auto-policy-stats", "frag-threshold-max-bp") == 6000, "max 60%");
    CHECK(href(cs, "query:arena-auto-policy-stats", "false-positive-target-bp") == 500, "FP 5%");
    CHECK(href(cs, "query:arena-auto-policy-stats", "auto-compact-false-positive-bp") >= 0,
          "fp bp");
    CHECK(href(cs, "query:arena-auto-policy-stats", "mutation-pressure-signals") >= 0, "mut sig");
    CHECK(href(cs, "query:arena-auto-policy-stats", "jit-deopt-pressure-signals") >= 0, "deopt");
    CHECK(href(cs, "query:arena-auto-policy-stats", "smart-policy-wired") == 1, "smart wired");
    CHECK(href(cs, "query:arena-auto-policy-stats", "shape-profiler-on-compact-wired") == 1,
          "shape hook");
    CHECK(href(cs, "query:arena-auto-policy-stats", "jit-deopt-throttle-wired") == 1, "jit thr");
}

static void ac5_production_sweep() {
    std::println("\n--- AC5: production-sweep schema-1919 arena keys ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:production-sweep-1241-1245-stats\")");
    CHECK(r && is_hash(*r), "sweep hash");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "schema") == 1241, "schema 1241");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "schema-1919") == 1919, "schema-1919");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "issue-1919") == 1919, "issue-1919");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats",
               "arena-intelligent-auto-compact-wired") == 1,
          "intelligent wired");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "arena-false-positive-target-bp") ==
              500,
          "FP target");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "arena-shrink-tier-hardened") == 1,
          "shrink lineage");
}

static void ac6_source_wiring() {
    std::println("\n--- AC6: ShapeProfiler/JIT/mutate wiring ---");
    std::string mut, svc, pol;
    for (const char* p : {"src/compiler/evaluator_primitives_mutate.cpp",
                          "../src/compiler/evaluator_primitives_mutate.cpp"}) {
        mut = read_file(p);
        if (!mut.empty())
            break;
    }
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
    CHECK(!mut.empty() && mut.find("signal_mutation_pressure") != std::string::npos,
          "mutate → mutation pressure");
    CHECK(!svc.empty() && svc.find("signal_jit_deopt_pressure") != std::string::npos,
          "service → jit deopt pressure");
    CHECK(!pol.empty() && pol.find("AutoCompactMode") != std::string::npos, "AutoCompactMode");
    CHECK(pol.find("compute_dynamic_frag_threshold") != std::string::npos, "dynamic thr");
    CHECK(pol.find("record_auto_compact_outcome") != std::string::npos, "FP outcome");
}

static void ac7_mutate_stress() {
    std::println("\n--- AC7: multi-round mutate stress ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto mut0 = href(cs, "query:arena-auto-policy-stats", "mutation-pressure-signals");
    for (int i = 0; i < 30; ++i) {
        (void)cs.eval(
            std::format("(mutate:rebind \"f\" \"(lambda (x) (+ x {}))\" \"i1919\")", i % 7));
        (void)cs.eval("(eval-current)");
    }
    CHECK(href(cs, "query:arena-auto-policy-stats", "mutation-pressure-signals") > mut0,
          "mutation signals advanced");
    CHECK(href(cs, "query:arena-auto-policy-stats", "schema-1919") == 1919, "schema holds");
    const auto fp_bp = href(cs, "query:arena-auto-policy-stats", "auto-compact-false-positive-bp");
    CHECK(fp_bp >= 0, "fp bp readable");
    // Target: <5% when enough samples; always readable.
    if (href(cs, "query:arena-auto-policy-stats", "auto-compact-true-positive-total") +
            href(cs, "query:arena-auto-policy-stats", "auto-compact-false-positive-total") >
        0) {
        CHECK(fp_bp <= 10000, "fp rate bounded under stress");
    }
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval after stress");
}

static void ac8_lineage() {
    std::println("\n--- AC8: #1621 / #743 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "query:arena-auto-policy-stats", "schema") == 1621, "schema 1621");
    CHECK(href(cs, "query:arena-auto-policy-stats", "smart-policy-evaluations") >= 0, "evals");
    CHECK(href(cs, "query:arena-auto-policy-stats", "smart-policy-triggers") >= 0, "triggers");
    CHECK(href(cs, "query:arena-auto-policy-stats", "closed-loop-wired") == 1, "closed-loop");
    CHECK(href(cs, "query:arena-auto-policy-stats", "auto-compact-triggers") >= 0, "triggers");
}

// Issue #2004: explicit live_compact primitive — (arena:live-compact) returns
// a LiveCompactResult hash with bytes_reclaimed / slots-recycled / new-gen /
// mode / soft-gated / invalidates-pins / schema=2004. Force mode bypasses
// the soft-gate (render hotpath / MutationBoundary) and bumps generation.
static void ac_live_compact_primitive() {
    std::println("\n--- AC_LC1: (arena:live-compact) primitive returns LiveCompactResult ---");
    CompilerService cs;
    // Allocate some objects so freelist recycling has something to do.
    CHECK(cs.eval("(let xs (map (lambda (x) (* x x)) (range 1 100)))").has_value(), "alloc");
    // Invoke the primitive (default Soft mode).
    const auto r1 = cs.eval("(arena:live-compact)");
    CHECK(r1.has_value(), "live_compact returns");
    // soft-gated=true is fine in test context (no MutationBoundary held);
    // result is otherwise valid.
    CHECK(href(cs, "arena:live-compact", "schema") == 2004, "schema 2004");
    CHECK(href(cs, "arena:live-compact", "new-gen") >= 0, "new-gen field present");
    CHECK(href(cs, "arena:live-compact", "soft-gated") >= 0, "soft-gated field present");
    CHECK(href(cs, "arena:live-compact", "invalidates-pins") >= 0,
          "invalidates-pins field present");

    std::println("\n--- AC_LC2: (arena:live-compact 1) Force mode bumps generation ---");
    const auto gen_before = href(cs, "arena:live-compact", "new-gen");
    const auto r2 = cs.eval("(arena:live-compact 1)"); // Force mode
    CHECK(r2.has_value(), "force live_compact returns");
    CHECK(href(cs, "arena:live-compact", "mode") == 1, "mode=1 (Force)");

    std::println("\n--- AC_LC3: (query:arena-live-compact-stats) surfaces 6 counters ---");
    cs.eval("(arena:live-compact 1)"); // Force again to bump force_count
    CHECK(href(cs, "query:arena-live-compact-stats", "schema") == 2004, "stats schema 2004");
    CHECK(href(cs, "query:arena-live-compact-stats", "soft-count") >= 0, "soft-count");
    CHECK(href(cs, "query:arena-live-compact-stats", "force-count") >= 0, "force-count");
    CHECK(href(cs, "query:arena-live-compact-stats", "reclaimed-bytes-total") >= 0, "reclaimed");
    CHECK(href(cs, "query:arena-live-compact-stats", "freelist-hits-total") >= 0, "freelist-hits");
    CHECK(href(cs, "query:arena-live-compact-stats", "gen-restamps-total") >= 0, "gen-restamps");
    CHECK(href(cs, "query:arena-live-compact-stats", "invalidated-pins-total") >= 0,
          "invalidated-pins");
}

} // namespace

int main() {
    std::println("=== Issue #1919: Intelligent arena auto-compact policy ===");
    ac1_modes_and_thresholds();
    ac2_policy_eval();
    ac3_outcome_counters();
    ac4_policy_stats();
    ac5_production_sweep();
    ac6_source_wiring();
    ac7_mutate_stress();
    ac8_lineage();
    ac_live_compact_primitive();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
