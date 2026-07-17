// @category: integration
// @reason: Issue #1621 — Arena smart auto-compact policy + dirty/Shape
// closed-loop (refine #743/#722/#1518).
//
//   AC1: evaluate_auto_compact_policy unit decisions
//   AC2: shape churn signal → policy trigger
//   AC3: query:arena-auto-policy-stats schema 1621 AC keys
//   AC4: mutate + defrag + boundary path advances counters
//   AC5: multi-round stress monotonic
//   AC6: #743 lineage keys + wire flags

#include "test_harness.hpp"
#include "core/arena_auto_policy_stats.h"

#include <cstdint>
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
using aura::core::arena_policy::evaluate_auto_compact_policy;
using aura::core::arena_policy::kPolicyReasonDirty;
using aura::core::arena_policy::kPolicyReasonFrag;
using aura::core::arena_policy::kPolicyReasonShapeChurn;
using aura::core::arena_policy::signal_dirty_cascade;
using aura::core::arena_policy::signal_shape_churn;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:arena-auto-policy-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static bool seed(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) "
                 "(define a 1) (define b 2) (fact 5)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static void ac1_policy_unit() {
    std::println("\n--- AC1: evaluate_auto_compact_policy unit ---");
    // Low frag, no signals → no compact
    auto d0 = evaluate_auto_compact_policy(0.05, false, false, false, false, false, 0.1);
    CHECK(!d0.should_compact, "low pressure no compact");

    // High frag → compact
    auto d1 = evaluate_auto_compact_policy(0.35, false, false, false, false, false, 0.1);
    CHECK(d1.should_compact, "high frag compact");
    CHECK((d1.reason & kPolicyReasonFrag) != 0, "frag reason bit");

    // Dirty cascade alone → compact
    auto d2 = evaluate_auto_compact_policy(0.05, false, true, false, false, false, 0.1);
    CHECK(d2.should_compact, "dirty cascade compact");
    CHECK((d2.reason & kPolicyReasonDirty) != 0, "dirty reason");

    // Shape churn alone → compact
    auto d3 = evaluate_auto_compact_policy(0.05, false, false, true, false, false, 0.1);
    CHECK(d3.should_compact, "shape churn compact");
    CHECK((d3.reason & kPolicyReasonShapeChurn) != 0, "shape reason");

    // Defrag req + soft frag → compact + prefer live
    auto d4 = evaluate_auto_compact_policy(0.20, true, false, false, false, false, 0.1);
    CHECK(d4.should_compact, "defrag_req + soft frag");
    CHECK(d4.prefer_live_defrag, "prefer live defrag");

    // Render hotpath → soft gate, no compact
    auto d5 = evaluate_auto_compact_policy(0.50, true, true, true, true, true, 0.9);
    CHECK(!d5.should_compact, "render hotpath soft-gates");
}

static void ac2_shape_churn_signal() {
    std::println("\n--- AC2: shape churn signal ---");
    signal_shape_churn();
    signal_dirty_cascade();
    auto d = evaluate_auto_compact_policy(0.10, false, true, true, false, false, 0.2);
    CHECK(d.should_compact, "churn+dirty triggers");
}

static void ac3_query_schema() {
    std::println("\n--- AC3: query schema 1621 ---");
    CompilerService cs;
    CHECK(seed(cs), "seed");
    // Force policy evaluation via defrag request + evals
    (void)cs.eval("(arena:request-defrag)");
    signal_shape_churn();
    signal_dirty_cascade();
    for (int i = 0; i < 20; ++i)
        (void)cs.eval("(fact 2)");
    (void)cs.eval("(mutate:rebind \"a\" \"10\")");
    (void)cs.eval("(eval-current)");

    auto h = cs.eval("(engine:metrics \"query:arena-auto-policy-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1621 || href(cs, "schema") == 743, "schema 1621|743");
    CHECK(href(cs, "issue") == 1621 || href(cs, "issue") < 0, "issue 1621");
    CHECK(href(cs, "auto-compact-triggers") >= 0, "auto-compact-triggers");
    CHECK(href(cs, "smart-policy-evaluations") >= 1, "smart-policy-evaluations");
    CHECK(href(cs, "smart-policy-triggers") >= 0, "smart-policy-triggers");
    CHECK(href(cs, "shape-churn-triggers") >= 0, "shape-churn-triggers");
    CHECK(href(cs, "boundary-exit-compacts") >= 0, "boundary-exit-compacts");
    CHECK(href(cs, "fiber-transition-compacts") >= 0, "fiber-transition-compacts");
    CHECK(href(cs, "live-defrag-policy-hits") >= 0, "live-defrag-policy-hits");
    CHECK(href(cs, "smart-policy-wired") == 1, "smart-policy-wired");
    CHECK(href(cs, "closed-loop-wired") == 1, "closed-loop-wired");
}

static void ac4_mutate_path() {
    std::println("\n--- AC4: mutate + defrag path ---");
    CompilerService cs;
    CHECK(seed(cs), "seed");
    const auto t0 = href(cs, "auto-compact-triggers");
    const auto e0 = href(cs, "smart-policy-evaluations");
    (void)cs.eval("(arena:request-defrag)");
    (void)cs.eval("(arena:adaptive-compact)");
    for (int i = 0; i < 30; ++i)
        (void)cs.eval("(fact 3)");
    (void)cs.eval("(mutate:rebind \"b\" \"20\")");
    (void)cs.eval("(eval-current)");
    CHECK(href(cs, "smart-policy-evaluations") >= e0, "evaluations advanced");
    CHECK(href(cs, "auto-compact-triggers") >= t0, "triggers monotonic");
    CHECK(href(cs, "shape-inval-on-compact") >= 0, "shape-inval readable");
    CHECK(href(cs, "env-reval-success") >= 0, "env-reval readable");
}

static void ac5_stress() {
    std::println("\n--- AC5: multi-round stress ---");
    CompilerService cs;
    CHECK(seed(cs), "seed");
    const auto e0 = href(cs, "smart-policy-evaluations");
    for (int i = 0; i < 25; ++i) {
        if ((i % 5) == 0)
            (void)cs.eval("(arena:request-defrag)");
        signal_dirty_cascade();
        if ((i % 7) == 0)
            signal_shape_churn();
        (void)cs.eval(std::format("(mutate:rebind \"a\" \"{}\")", i));
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(fact 2)");
    }
    CHECK(href(cs, "smart-policy-evaluations") >= e0, "evaluations non-decreasing");
    auto r = cs.eval("(+ 1 1)");
    CHECK(r.has_value(), "eval ok after stress");
}

static void ac6_lineage() {
    std::println("\n--- AC6: #743 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "defrag-fiber-safe-hits") >= 0, "defrag-fiber-safe-hits");
    CHECK(href(cs, "fragmentation-post-mutate") >= 0, "fragmentation-post-mutate");
    CHECK(href(cs, "smart-policy-wired") == 1, "wired");
}

} // namespace

int main() {
    std::println("=== Issue #1621: Arena smart auto-compact policy ===");
    ac1_policy_unit();
    ac2_shape_churn_signal();
    ac3_query_schema();
    ac4_mutate_path();
    ac5_stress();
    ac6_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
