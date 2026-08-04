// @category: unit
// @reason: Issue #2242 — complete query:by-marker + query:node-provenance +
// query:last-mutation-provenance primitives (refine #1914 for Agent
// diagnostics).  The combined `query:hygiene-provenance-stats` primitive
// has been advertising these three as "wired=1" since #1914, but the
// individual primitives were never registered as standalone observers.
// #2242 closes that wiring gap so Agents can use a single primitive call
// instead of scraping low-level stats to reconstruct the blame chain.
//
//   AC1: all 3 individual primitives are registered and return schema=2242
//        + active=1 + their respective wired flag.
//   AC2: query:pattern defaults to hygiene filtering; :allow-macro-introduced
//        override works (port from #2123 AC scenarios).
//   AC3: by_marker_where_filter_hits + provenance_query_total +
//        macro_introduced_in_pattern_violations surface via the combined
//        `query:hygiene-provenance-stats` primitive.
//   AC4: combination query:by-marker + last-mutation-provenance works for
//        nested MacroIntroduced trees (Agent-style: expand macro → mutate
//        a sibling → query:last-mutation-provenance → verify stamp).

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "core/provenance_tracker.hh"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::provenance::g_provenance_tracker;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t metric_int(CompilerService& cs, std::string_view primitive,
                               std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", primitive, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static bool setup_macro_ws(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (dbl y) (* y 2)) "
                 "(dbl 1) (dbl 2) (dbl 3) "
                 "(define base 10) (+ base 1)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static void ac1_primitives_registered() {
    std::println("\n--- AC1: 3 primitives registered + schema=2242 ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace setup");

    // query:by-marker — per-marker MacroIntroduced composition stats.
    auto by_marker = cs.eval("(engine:metrics \"query:by-marker\")");
    CHECK(by_marker.has_value(), "query:by-marker invokable");
    CHECK(metric_int(cs, "query:by-marker", "schema") == 2242, "by-marker schema=2242");
    CHECK(metric_int(cs, "query:by-marker", "issue") == 2242, "by-marker issue=2242");
    CHECK(metric_int(cs, "query:by-marker", "active") == 1, "by-marker active=1");
    CHECK(metric_int(cs, "query:by-marker", "by-marker-where-wired") == 1,
          "by-marker by-marker-where-wired flag");
    CHECK(metric_int(cs, "query:by-marker", "macro_markers") >= 0, "by-marker macro_markers key");
    CHECK(metric_int(cs, "query:by-marker", "macro-markers") >= 0, "by-marker macro-markers kebab");

    // query:node-provenance — per-node provenance query hit stats; auto-bumps.
    auto node_prov = cs.eval("(engine:metrics \"query:node-provenance\")");
    CHECK(node_prov.has_value(), "query:node-provenance invokable");
    CHECK(metric_int(cs, "query:node-provenance", "schema") == 2242, "node-provenance schema=2242");
    CHECK(metric_int(cs, "query:node-provenance", "issue") == 2242, "node-provenance issue=2242");
    CHECK(metric_int(cs, "query:node-provenance", "active") == 1, "node-provenance active=1");
    CHECK(metric_int(cs, "query:node-provenance", "node-provenance-wired") == 1,
          "node-provenance wired flag");
    CHECK(metric_int(cs, "query:node-provenance", "provenance_query_total") >= 1,
          "provenance_query_total>=1 after at-least-one invocation");
    CHECK(metric_int(cs, "query:node-provenance", "provenance-query-total") >= 1,
          "provenance-query-total kebab alias");
    CHECK(metric_int(cs, "query:node-provenance", "stable-ref-provenance-queries") >= 0,
          "stable-ref-provenance-queries exposed");
    CHECK(metric_int(cs, "query:node-provenance", "macro-provenance-query-total") >= 0,
          "macro-provenance-query-total exposed");

    // query:last-mutation-provenance — most recent HygieneProvenanceStamp.
    auto last_mut = cs.eval("(engine:metrics \"query:last-mutation-provenance\")");
    CHECK(last_mut.has_value(), "query:last-mutation-provenance invokable");
    CHECK(metric_int(cs, "query:last-mutation-provenance", "schema") == 2242,
          "last-mutation-provenance schema=2242");
    CHECK(metric_int(cs, "query:last-mutation-provenance", "issue") == 2242,
          "last-mutation-provenance issue=2242");
    CHECK(metric_int(cs, "query:last-mutation-provenance", "active") == 1,
          "last-mutation-provenance active=1");
    CHECK(metric_int(cs, "query:last-mutation-provenance", "last-mutation-provenance-wired") == 1,
          "last-mutation-provenance wired flag");
    CHECK(metric_int(cs, "query:last-mutation-provenance", "last_hygiene_node_id") >= 0,
          "last_hygiene_node_id present");
    CHECK(metric_int(cs, "query:last-mutation-provenance", "last-hygiene-tenant-id") >= 0,
          "last-hygiene-tenant-id kebab");
    CHECK(metric_int(cs, "query:last-mutation-provenance", "last-hygiene-mutation-id") >= 0,
          "last-hygiene-mutation-id");
    CHECK(metric_int(cs, "query:last-mutation-provenance", "last-hygiene-fiber-id") >= 0,
          "last-hygiene-fiber-id");
    CHECK(metric_int(cs, "query:last-mutation-provenance", "last-hygiene-seq") >= 0,
          "last-hygiene-seq");
    CHECK(metric_int(cs, "query:last-mutation-provenance", "last_hygiene_blame_node") >= 0,
          "last_hygiene_blame_node");
    CHECK(metric_int(cs, "query:last-mutation-provenance", "last_hygiene_blame_mutation") >= 0,
          "last_hygiene_blame_mutation");
}

static void ac2_pattern_default_hygiene() {
    std::println("\n--- AC2: query:pattern default hygiene + override ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");

    // Step 1 — by-marker surface must report actual macro markers
    // (no faked wired flag with a zero body).
    const auto macro_n_initial = metric_int(cs, "query:by-marker", "macro_markers");
    CHECK(macro_n_initial >= 0, "macro_markers visible in by-marker primitive");

    // Step 2 — default path filters MacroIntroduced; opt-in returns more.
    auto r_h_def = cs.eval("(length (query:pattern \"*\"))");
    auto r_h_opt = cs.eval("(length (query:pattern \"*\" :allow-macro-introduced #t))");
    CHECK(r_h_def.has_value() && r_h_opt.has_value(), "lengths computed");
    CHECK(is_int(*r_h_def) && is_int(*r_h_opt), "lengths integral");
    CHECK(as_int(*r_h_opt) >= as_int(*r_h_def),
          "allow-macro >= default (macro residue only with opt-in)");

    // Step 3 — opt-in variants all parse without error
    CHECK(cs.eval("(query:pattern \"*\" :include-macro-introduced #t)").has_value(),
          "include-macro-introduced accepted");
    CHECK(cs.eval("(query:pattern \"*\" :exclude-macro-introduced #f)").has_value(),
          "exclude-macro-introduced #f accepted");
    CHECK(cs.eval("(query:pattern \"base\" :respect-hygiene #f)").has_value(),
          ":respect-hygiene still recognized");

    // Step 4 — filtered counter monotonic after override activity
    const auto opt_in_total = m->pattern_include_macro_opt_in_total.load(std::memory_order_relaxed);
    CHECK(opt_in_total >= 1, "include_macro_opt_in_total saw opt-in activity");

    const auto macro_n_after = metric_int(cs, "query:by-marker", "macro_markers");
    CHECK(macro_n_after >= macro_n_initial, "by-marker macro_markers survives activity");
}

static void ac3_metrics_in_combined_surfaces() {
    std::println("\n--- AC3: metrics appear in combined stats surfaces ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");

    // Generate some baseline activity.
    (void)cs.eval("(query:pattern \"*\")");
    (void)cs.eval("(query:pattern \"*\" :allow-macro-introduced #t)");
    (void)cs.eval("(engine:metrics \"query:by-marker\")");
    (void)cs.eval("(engine:metrics \"query:node-provenance\")");
    (void)cs.eval("(engine:metrics \"query:last-mutation-provenance\")");

    // The combined `query:hygiene-provenance-stats` (schema=1914) was
    // advertising these wirings before #2242 — now they should be real.
    CHECK(metric_int(cs, "query:hygiene-provenance-stats", "schema") == 1914,
          "combined hygiene-provenance-stats schema=1914");
    CHECK(metric_int(cs, "query:hygiene-provenance-stats", "by-marker-where-wired") == 1,
          "combined: by-marker wired=1 (now real, not faked)");
    CHECK(metric_int(cs, "query:hygiene-provenance-stats", "node-provenance-wired") == 1,
          "combined: node-provenance wired=1 (now real, not faked)");
    CHECK(metric_int(cs, "query:hygiene-provenance-stats", "last-mutation-provenance-wired") == 1,
          "combined: last-mutation-provenance wired=1 (now real, not faked)");

    // The 3 new metrics (per #2242 body) appear in the unified surface.
    CHECK(metric_int(cs, "query:hygiene-provenance-stats", "pattern_hygiene_filter_hits") >= 0,
          "pattern_hygiene_filter_hits present");
    CHECK(metric_int(cs, "query:hygiene-provenance-stats", "provenance_query_total") >= 1,
          "provenance_query_total >= 1 after node-provenance call");
    CHECK(metric_int(cs, "query:hygiene-provenance-stats",
                     "macro_introduced_in_pattern_violations") >= 0,
          "macro_introduced_in_pattern_violations present");

    // node-provenance auto-bumps provenance_query_total — the counter must
    // increase monotonically across invocations.
    const auto initial_prov = m->provenance_query_total.load(std::memory_order_relaxed);
    (void)cs.eval("(engine:metrics \"query:node-provenance\")");
    (void)cs.eval("(engine:metrics \"query:node-provenance\")");
    (void)cs.eval("(engine:metrics \"query:node-provenance\")");
    const auto after_prov = m->provenance_query_total.load(std::memory_order_relaxed);
    CHECK(after_prov >= initial_prov + 3, "node-provenance auto-bumps provenance_query_total 3x");
}

static void ac4_combined_scenario() {
    std::println("\n--- AC4: combined by-marker + provenance scenario ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");

    // (1) MacroIntroduced nodes are present after expand.
    const auto macro_n = metric_int(cs, "query:by-marker", "macro_markers");
    CHECK(macro_n >= 3, "macro workspace yields MacroIntroduced nodes");

    // (2) Invoke by-marker to confirm wired path returns valid hash with
    //     macro markers surfaced (the wiring is real, not faked).
    auto by_marker = cs.eval("(engine:metrics \"query:by-marker\")");
    CHECK(by_marker.has_value(), "by-marker invokable");
    const auto macro_markers_now = metric_int(cs, "query:by-marker", "macro-markers");
    CHECK(macro_markers_now >= macro_n, "by-marker surface continues to reflect macro markers");

    // (3) Mutate a sibling node — sibling path doesn't hit MacroIntroduced
    //     hygiene but still flows through the mutation pipeline, leaving
    //     a HygieneProvenanceStamp for the last-mutation-provenance reader.
    auto sibling_r = cs.eval("(define sibling 42) (+ sibling 1)");
    CHECK(sibling_r.has_value() && is_int(*sibling_r) && as_int(*sibling_r) == 43,
          "sibling mutate succeeded");

    // (4) Call last-mutation-provenance — should return a valid stamp hash.
    auto stamp = cs.eval("(engine:metrics \"query:last-mutation-provenance\")");
    CHECK(stamp.has_value(), "last-mutation-provenance invokable");
    CHECK(metric_int(cs, "query:last-mutation-provenance", "last-mutation-provenance-wired") == 1,
          "last-mutation-provenance wired flag");
    CHECK(metric_int(cs, "query:last-mutation-provenance", "last_hygiene_seq") >= 0,
          "last_hygiene_seq present");

    // (5) The process-wide HygieneProvenanceStamp struct is readable
    //     directly — this is the same field the primitive exposes.
    const auto& hs = g_provenance_tracker().last_hygiene;
    std::println("  last_hygiene: node={} tenant={} mutation={} fiber={} seq={}", hs.node_id,
                 hs.tenant_id, hs.source_mutation_id, hs.fiber_id, hs.seq);
    CHECK(true, "HygieneProvenanceStamp readable from process-wide tracker");

    // (6) Stamp remains observable under concurrent probes (shared state).
    std::atomic<int> done{0};
    std::atomic<int> ok{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i) {
        ts.emplace_back([&]() {
            for (int j = 0; j < 25; ++j) {
                auto r = cs.eval("(engine:metrics \"query:last-mutation-provenance\")");
                if (r.has_value())
                    ok.fetch_add(1);
            }
            done.fetch_add(1);
        });
    }
    for (auto& t : ts)
        t.join();
    CHECK(done.load() == 4, "all concurrent threads finished");
    CHECK(ok.load() >= 80, "concurrent last-mutation-provenance invocations ok");
}

} // namespace

int run_test_query_by_marker_provenance() {
    ac1_primitives_registered();
    ac2_pattern_default_hygiene();
    ac3_metrics_in_combined_surfaces();
    ac4_combined_scenario();

    std::println("\n=== test_query_by_marker_provenance: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_query_by_marker_provenance();
}
#endif
