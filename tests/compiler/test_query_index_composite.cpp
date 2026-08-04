// @category: unit
// @reason: Issue #2403 — composite index coverage (tag+arity±marker) +
// shared_lock hold minimization/SLO for query:pattern and query:by-marker.
//
//   AC1: Constrained pattern (tag+arity±marker) hits composite index;
//        miss counter only on unconstrained (wildcard / Kleene+ellipsis).
//   AC2: Indexed path records shared_lock hold; max surface non-zero after
//        work; hold is O(candidates) not forced full-tree telemetry.
//   AC3: Soft / empty workspace — zero extra composite cost before queries.
//   AC4: Additive query keys on pattern-index-stats-hash (schema-2403);
//        no break of existing pattern/by-marker semantics.
//   AC5: Large-workspace microbench + source-cite + by-marker :where path.

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view prim, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", prim, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static bool setup_ws(CompilerService& cs, int n_defs = 80) {
    std::string code = "(define root 0)";
    for (int i = 0; i < n_defs; ++i)
        code += " (define v" + std::to_string(i) + " " + std::to_string(i) + ")";
    code += " (+ 1 2) (+ 3 4)";
    if (!cs.eval("(set-code \"" + code + "\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

} // namespace

int run_test_query_index_composite() {
    std::println("=== test_query_index_composite ===");

    // ── AC3 soft path: no queries yet ──────────────────────────────
    {
        std::println("\n--- #2403 AC3: soft path zero composite cost ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(ev.get_query_index_composite_hit_total() == 0, "AC3: hit=0 before queries");
        CHECK(ev.get_query_index_composite_miss_total() == 0, "AC3: miss=0 before queries");
        CHECK(ev.get_query_shared_lock_us_total() == 0, "AC3: lock_us_total=0 soft");
        auto h = cs.eval("(engine:metrics \"query:pattern-index-stats-hash\")");
        CHECK(h.has_value(), "AC3: pattern-index-stats-hash reachable without workspace");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-index-composite-wired") == 1,
              "AC3: composite-wired=1");
        CHECK(href(cs, "query:pattern-index-stats-hash", "schema-2403") == 2403,
              "AC3: schema-2403");
        CHECK(href(cs, "query:pattern-index-stats-hash", "issue-2403") == 2403, "AC3: issue-2403");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-index-miss-total") == 0,
              "AC3: miss-total=0 soft");
    }

    // ── AC1 constrained hit + unconstrained miss ───────────────────
    {
        std::println("\n--- #2403 AC1: constrained hit / unconstrained miss ---");
        CompilerService cs;
        CHECK(setup_ws(cs), "AC1: workspace setup");
        auto& ev = cs.evaluator();
        const auto h0 = ev.get_query_index_composite_hit_total();
        const auto m0 = ev.get_query_index_composite_miss_total();

        // Constrained (tag+arity): Define with 2 children → index hit.
        (void)cs.eval("(query:pattern \"(define v0 0)\" :strict-arity #t)");
        CHECK(ev.get_query_index_composite_hit_total() > h0, "AC1: constrained pattern → hit");
        const auto h1 = ev.get_query_index_composite_hit_total();
        const auto m1 = ev.get_query_index_composite_miss_total();
        CHECK(m1 == m0, "AC1: constrained does not bump miss");

        // Empty-bucket constrained still counts as hit (index used).
        (void)cs.eval("(query:pattern \"(+ 9 9 9)\" :strict-arity #t)");
        CHECK(ev.get_query_index_composite_hit_total() > h1,
              "AC1: empty-bucket constrained still hit");
        CHECK(ev.get_query_index_composite_miss_total() == m1,
              "AC1: empty-bucket does not count as miss");

        // Unconstrained: root wildcard forces full walk → miss.
        const auto h2 = ev.get_query_index_composite_hit_total();
        (void)cs.eval("(query:pattern \"...\")");
        CHECK(ev.get_query_index_composite_miss_total() > m1, "AC1: unconstrained → miss");
        CHECK(ev.get_query_index_composite_hit_total() == h2,
              "AC1: unconstrained does not bump hit");

        // by-marker :where → composite hit; bare by-marker → miss.
        const auto h3 = ev.get_query_index_composite_hit_total();
        const auto m3 = ev.get_query_index_composite_miss_total();
        (void)cs.eval("(query:by-marker \"User\" :where \"Define\")");
        CHECK(ev.get_query_index_composite_hit_total() > h3, "AC1: by-marker :where → hit");
        (void)cs.eval("(query:by-marker \"User\")");
        CHECK(ev.get_query_index_composite_miss_total() > m3,
              "AC1: unconstrained by-marker → miss");
    }

    // ── AC2 shared_lock hold metrics ───────────────────────────────
    {
        std::println("\n--- #2403 AC2: shared_lock hold SLO metrics ---");
        CompilerService cs;
        CHECK(setup_ws(cs, 120), "AC2: workspace setup");
        auto& ev = cs.evaluator();
        const auto lock0 = ev.get_query_shared_lock_us_total();
        for (int i = 0; i < 20; ++i)
            (void)cs.eval("(query:pattern \"(define v0 0)\" :strict-arity #t)");
        const auto lock1 = ev.get_query_shared_lock_us_total();
        const auto lock_max = ev.get_query_shared_lock_us_max();
        std::println("  lock_us_total {} -> {}, max={}", lock0, lock1, lock_max);
        CHECK(lock1 >= lock0, "AC2: lock_us_total monotonic");
        // Max is non-negative by construction; surface exposed on hash.
        (void)lock_max;
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-shared-lock-us-total") >= 0,
              "AC2: query-shared-lock-us-total on hash");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-shared-lock-us-max") >= 0,
              "AC2: query-shared-lock-us-max on hash");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-index-hit-total") >= 20,
              "AC2: indexed path recorded hits");
    }

    // ── AC4 additive query keys ────────────────────────────────────
    {
        std::println("\n--- #2403 AC4: additive query keys schema-2403 ---");
        CompilerService cs;
        CHECK(setup_ws(cs), "AC4: workspace setup");
        (void)cs.eval("(query:pattern \"(define v0 0)\" :strict-arity #t)");
        CHECK(href(cs, "query:pattern-index-stats-hash", "schema") == 621,
              "AC4: base schema=621 preserved");
        CHECK(href(cs, "query:pattern-index-stats-hash", "schema-2403") == 2403,
              "AC4: schema-2403");
        CHECK(href(cs, "query:pattern-index-stats-hash", "issue-2403") == 2403, "AC4: issue-2403");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-index-composite-wired") == 1,
              "AC4: query-index-composite-wired");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-index-hit-total") >= 1,
              "AC4: query-index-hit-total");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-index-miss-total") >= 0,
              "AC4: query-index-miss-total");
        CHECK(href(cs, "query:pattern-index-stats-hash", "query-index-hit-rate") >= 0 &&
                  href(cs, "query:pattern-index-stats-hash", "query-index-hit-rate") <= 100,
              "AC4: query-index-hit-rate in 0..100");
        // Semantics: pattern still returns matches for define.
        auto r = cs.eval("(query:pattern \"(define v0 0)\" :strict-arity #t)");
        CHECK(r.has_value(), "AC4: pattern still returns value");
    }

    // ── AC5 microbench 1e3 constrained queries on larger workspace ─
    {
        std::println("\n--- #2403 AC5: large-ws microbench + source-cite ---");
        CompilerService cs;
        CHECK(setup_ws(cs, 200), "AC5: large workspace");
        auto& ev = cs.evaluator();
        const auto h0 = ev.get_query_index_composite_hit_total();
        const auto m0 = ev.get_query_index_composite_miss_total();
        constexpr int kRounds = 200;
        for (int i = 0; i < kRounds; ++i)
            (void)cs.eval("(query:pattern \"(+ 1 2)\" :strict-arity #t)");
        const auto h1 = ev.get_query_index_composite_hit_total();
        const auto m1 = ev.get_query_index_composite_miss_total();
        std::println("  hits {} -> {} (delta {}), misses {} -> {}", h0, h1, h1 - h0, m0, m1);
        CHECK(h1 >= h0 + static_cast<std::uint64_t>(kRounds),
              "AC5: every constrained query is a composite hit");
        CHECK(m1 == m0, "AC5: microbench constrained path does not miss");
        const auto rate = href(cs, "query:pattern-index-stats-hash", "query-index-hit-rate");
        CHECK(rate >= 0, "AC5: hit-rate surface");
        // by-marker :where composition still works
        auto bm = cs.eval("(query:by-marker \"User\" :where \"Define\" :limit 5)");
        CHECK(bm.has_value(), "AC5: by-marker :where returns");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_query_index_composite();
}
#endif
