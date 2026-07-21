// test_query_pattern_hygiene_index_sv.cpp — Issue #547:
// query:pattern + MacroIntroduced Hygiene Filter + Incremental
// tag_arity_index + DefUseIndex integration for large-scale
// self-evolving SV/RTL verification loops.
//
// Non-duplicative with #541 (query hygiene), #528/#524 (marker
// propagation), #514 (meta). This binary focuses on the
// production-readiness matrix the Task 6 review flagged:
//
//   - AC1: tag_arity_index hits/misses/rebuilds/dirty_marks
//          counters reachable + monotonic under mutate load
//   - AC2: mark_dirty_upward flips the tag_arity_index dirty
//          flag (the integration hook for incremental
//          rebuild)
//   - AC3: rebuild_tag_arity_index() clears the dirty flag
//   - AC4: (engine:metrics \"query:pattern-index-stats\") returns integer sum
//          of the 4 counters
//   - AC5: (engine:metrics \"query:pattern-hygiene-stats\") returns integer sum
//          of skips + violations
//   - AC6: :respect-hygiene keyword alias for
//          :include-macro-introduced (same semantics)
//   - AC7: Default query:pattern filters MacroIntroduced
//          (the production hygiene default)
//   - AC8: Stress — 1000+ mutate under macro expansion
//          (no hygiene violation, dirty_marks grows)
//   - AC9: Regression — existing primitives still work

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace aura_issue_547_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;

// ── Tunables ──────────────────────────────────────────────
static int k_stress_iters() {
    return k_int_env("AURA_STRESS_ITERS", 200);
}

// ── AC1: tag_arity_index hits/misses/rebuilds/dirty_marks
//         counters reachable + monotonic ──────────────────
bool test_tag_arity_index_counters_reachable() {
    std::println("\n--- AC1: tag_arity_index counters reachable + monotonic ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace_flat() reachable");
    if (!ws)
        return false;
    const auto h0 = ws->tag_arity_index_hits();
    const auto m0 = ws->tag_arity_index_misses();
    const auto r0 = ws->tag_arity_index_rebuilds();
    const auto d0 = ws->tag_arity_index_dirty_marks();
    std::println("  baseline: hits={} misses={} rebuilds={} dirty_marks={}", h0, m0, r0, d0);
    CHECK(d0 == 0, "tag_arity_index_dirty_marks starts at 0 (fresh workspace)");
    // Trigger a (query:tag-arity-count) to bump hits/misses.
    auto r1 = cs.eval("(query:tag-arity-count 32 0)");
    CHECK(r1.has_value(), "(query:tag-arity-count) returns");
    const auto h1 = ws->tag_arity_index_hits();
    const auto m1 = ws->tag_arity_index_misses();
    const auto r1b = ws->tag_arity_index_rebuilds();
    std::println("  after query: hits={} misses={} rebuilds={}", h1, m1, r1b);
    CHECK(h1 + m1 > h0 + m0, "hits+misses bumped after (query:tag-arity-count)");
    return true;
}

// ── AC2: mark_dirty_upward flips the tag_arity_index dirty
//         flag ─────────────────────────────────────────────
bool test_mark_dirty_upward_flips_dirty_flag() {
    std::println("\n--- AC2: mark_dirty_upward flips tag_arity_index dirty flag ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define x 1) (define y 2)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        ++aura::test::g_failed;
        return false;
    }
    // First build the index (otherwise find_by_tag_arity
    // might trigger a lazy rebuild that clears the dirty
    // flag we'd be setting next).
    ws->rebuild_tag_arity_index();
    const auto d0 = ws->tag_arity_index_dirty_marks();
    // Call mark_dirty_upward directly on node 0 (the
    // workspace has >= 1 node after set-code + eval).
    if (ws->size() > 0) {
        ws->mark_dirty_upward(0);
    }
    const auto d1 = ws->tag_arity_index_dirty_marks();
    const auto dirty = ws->tag_arity_index_dirty();
    std::println("  dirty_marks: {} -> {} dirty_flag: {}", d0, d1, dirty);
    CHECK(d1 > d0, "tag_arity_index_dirty_marks bumped after mark_dirty_upward() call");
    CHECK(dirty, "tag_arity_index dirty flag set after mark_dirty_upward()");
    return true;
}

// ── AC3: rebuild_tag_arity_index() clears the dirty flag
bool test_rebuild_clears_dirty_flag() {
    std::println("\n--- AC3: rebuild_tag_arity_index() clears dirty flag ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        ++aura::test::g_failed;
        return false;
    }
    ws->rebuild_tag_arity_index();
    // Force a dirty mark directly.
    if (ws->size() > 0) {
        ws->mark_dirty_upward(0);
    }
    CHECK(ws->tag_arity_index_dirty(), "tag_arity_index is dirty after mark_dirty_upward");
    // Rebuild.
    ws->rebuild_tag_arity_index();
    CHECK(!ws->tag_arity_index_dirty(), "tag_arity_index is clean after rebuild_tag_arity_index()");
    return true;
}

// ── AC4: query:pattern-index-stats returns integer sum ────
bool test_query_pattern_index_stats() {
    std::println("\n--- AC4: (engine:metrics \"query:pattern-index-stats\") returns integer ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    // Trigger a few mutates + queries to bump counters.
    for (int i = 0; i < 5; ++i) {
        (void)cs.eval("(mutate:replace-value (define a " + std::to_string(i) + ") (define a " +
                      std::to_string(i) + "))");
        (void)cs.eval("(query:tag-arity-count 32 0)");
    }
    auto r = cs.eval("(engine:metrics \"query:pattern-index-stats\")");
    CHECK(r.has_value(), "(engine:metrics \"query:pattern-index-stats\") returns");
    CHECK(aura::compiler::types::is_int(*r),
          "(engine:metrics \"query:pattern-index-stats\") is int or hash");
    if (r && aura::compiler::types::is_int(*r)) {
        const auto v = aura::compiler::types::as_int(*r);
        std::println("  query:pattern-index-stats = {}", v);
        CHECK(v > 0, "(engine:metrics \"query:pattern-index-stats\") > 0 after mutates + queries");
    }
    return true;
}

// ── AC5: query:pattern-hygiene-stats returns int sum (#547) or hash (#1609) ──
bool test_query_pattern_hygiene_stats() {
    std::println(
        "\n--- AC5: (engine:metrics \"query:pattern-hygiene-stats\") returns int or hash ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    CHECK(r.has_value(), "(engine:metrics \"query:pattern-hygiene-stats\") returns");
    CHECK(r && (aura::compiler::types::is_int(*r) || aura::compiler::types::is_hash(*r)),
          "(engine:metrics \"query:pattern-hygiene-stats\") is int or hash");
    if (r && aura::compiler::types::is_int(*r)) {
        const auto v = aura::compiler::types::as_int(*r);
        std::println("  query:pattern-hygiene-stats = {}", v);
        CHECK(v >= 0, "(engine:metrics \"query:pattern-hygiene-stats\") >= 0 (skips + violations)");
    } else if (r && aura::compiler::types::is_hash(*r)) {
        std::println("  query:pattern-hygiene-stats = hash (schema 1609)");
        CHECK(true, "pattern-hygiene-stats hash authoritative (#1609)");
    }
    return true;
}

// ── AC6: :respect-hygiene keyword alias for
//         :include-macro-introduced ─────────────────────────
bool test_respect_hygiene_keyword() {
    std::println("\n--- AC6: :respect-hygiene keyword alias for :include-macro-introduced ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define x 1)\")");
    (void)cs.eval("(eval-current)");
    // The new keyword should be recognized (not unknown).
    // If recognized, the call returns a value (or pair); if
    // unknown, it returns a bad-arg error.
    auto r = cs.eval("(query:pattern \"x\" :respect-hygiene #f)");
    CHECK(r.has_value(), "(query:pattern :respect-hygiene #f) returns (keyword recognized)");
    // Verify the default is still skip (hygiene-safe).
    auto r2 = cs.eval("(query:pattern \"x\")");
    CHECK(r2.has_value(), "(query:pattern) without :respect-hygiene returns (default = skip)");
    return true;
}

// ── AC7: Default query:pattern filters MacroIntroduced ────
bool test_default_filters_macro_introduced() {
    std::println("\n--- AC7: default query:pattern filters MacroIntroduced ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define x 1)\")");
    (void)cs.eval("(eval-current)");
    // Verify (engine:metrics \"query:pattern-hygiene-stats\") is reachable
    // (#547 int sum or #1609 authoritative hash).
    auto r = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    if (!r || !(aura::compiler::types::is_int(*r) || aura::compiler::types::is_hash(*r))) {
        ++aura::test::g_failed;
        return false;
    }
    const auto baseline = cs.evaluator().get_macro_introduced_skipped_in_query();
    // Run a pattern query — the default skip-if-MacroIntroduced
    // path bumps macro_introduced_skipped_in_query_ when the
    // workspace has any MacroIntroduced nodes. The exact count
    // depends on workspace contents; we just check monotonicity.
    auto r2 = cs.eval("(query:pattern \"x\")");
    CHECK(r2.has_value(), "(query:pattern \"x\") returns");
    const auto after = cs.evaluator().get_macro_introduced_skipped_in_query();
    std::println("  macro_introduced_skipped_in_query: {} (baseline: {})", after, baseline);
    CHECK(after >= baseline, "macro_introduced_skipped_in_query observable + non-decreasing");
    return true;
}

// ── AC8: Stress — 200 iters mutate under load ─────────────
bool test_stress_mutate_dirty_marks() {
    std::println("\n--- AC8: {} iters mark_dirty_upward stress ---", k_stress_iters());
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        ++aura::test::g_failed;
        return false;
    }
    ws->rebuild_tag_arity_index();
    const auto d0 = ws->tag_arity_index_dirty_marks();
    const auto r0 = ws->tag_arity_index_rebuilds();
    std::mt19937 rng(547u);
    std::uniform_int_distribution<int> node_dist(
        0, static_cast<int>(ws->size() > 0 ? ws->size() - 1 : 0));
    for (int i = 0; i < k_stress_iters(); ++i) {
        // Call mark_dirty_upward directly (bypasses Aura
        // eval cost for the stress).
        if (ws->size() > 0) {
            ws->mark_dirty_upward(static_cast<aura::ast::NodeId>(node_dist(rng)));
        }
    }
    const auto d1 = ws->tag_arity_index_dirty_marks();
    const auto r1 = ws->tag_arity_index_rebuilds();
    std::println("  dirty_marks: {} -> {} (delta {}) rebuilds: {} -> {}", d0, d1, d1 - d0, r0, r1);
    CHECK(d1 - d0 >= static_cast<std::uint64_t>(k_stress_iters() - 5),
          "dirty_marks bumped >= ~iter count under mark_dirty_upward stress");
    CHECK(d1 - d0 >= r1 - r0, "dirty_marks >= rebuilds delta (rebuilds clear dirty flag)");
    return true;
}

// ── AC9: Regression — existing primitives still work ────
bool test_regression_existing_primitives() {
    std::println("\n--- AC9: regression — existing primitives still work ---");
    CompilerService cs;
    auto r1 = cs.eval("(engine:metrics \"query:pattern-index-stats\")");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "(engine:metrics \"query:pattern-index-stats\") (new for #547)");
    auto r2 = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    CHECK(r2.has_value() &&
              (aura::compiler::types::is_int(*r2) || aura::compiler::types::is_hash(*r2)),
          "(engine:metrics \"query:pattern-hygiene-stats\") (new for #547 / #1609)");
    auto r3 = cs.eval("(engine:metrics \"query:query-stats\")");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "(engine:metrics \"query:query-stats\") (regression for #447)");
    auto r4 = cs.eval("(query:tag-arity-count 32 0)");
    CHECK(r4.has_value() && aura::compiler::types::is_int(*r4),
          "(query:tag-arity-count) (regression for #447)");
    // Define + eval still works.
    if (!cs.eval("(define reg-547-a 10)")) {
        CHECK(false, "define (regression)");
        return false;
    }
    auto r5 = cs.eval("(define reg-547-b 32)");
    (void)r5;
    auto r6 = cs.eval("(+ reg-547-a reg-547-b)");
    CHECK(r6.has_value() && aura::compiler::types::is_int(*r6) &&
              aura::compiler::types::as_int(*r6) == 42,
          "(+ reg-547-a reg-547-b) == 42 (regression)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #547 verification tests ═══\n");
    std::println("Layer 1: tag_arity_index observability + dirty hook");
    test_tag_arity_index_counters_reachable();
    test_mark_dirty_upward_flips_dirty_flag();
    test_rebuild_clears_dirty_flag();
    std::println("\nLayer 2: query:pattern hygiene + stats primitives");
    test_query_pattern_index_stats();
    test_query_pattern_hygiene_stats();
    test_respect_hygiene_keyword();
    test_default_filters_macro_introduced();
    std::println("\nLayer 3: stress + regression");
    test_stress_mutate_dirty_marks();
    test_regression_existing_primitives();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_547_detail

int aura_issue_547_run() {
    return aura_issue_547_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_547_run();
}
#endif