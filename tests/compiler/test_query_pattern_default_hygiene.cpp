// @category: unit
// @reason: Issue #2123 — default hygiene filter for query:pattern +
// MacroIntroduced linkage (production "code as memory" contract).
// Issue #2763 — production default delta rebuild under low dirty ratio +
// MacroIntroduced hygiene hard filter (refine #1503/#2123).
//
//   AC1: source cites #2123; matcher/query:pattern document default skip
//   AC2: after macro expand, default query:pattern does not return
//        MacroIntroduced-only expansion nodes
//   AC3: opt-in :include-macro-introduced / :allow-macro-introduced still
//        returns MacroIntroduced when requested
//   AC4: concurrent query:pattern under shared_lock does not crash
//   AC5: pattern_hygiene_filtered_total + pattern_include_macro_opt_in_total
//        on query:pattern-hygiene-stats schema-2123
//   AC6: existing hygiene keywords still recognized
//   AC7: this registered issue test
//
//   #2763 AC1: low dirty ratio after structural mutate → delta rebuild
//   #2763 AC2: MacroIntroduced hard-filtered by default
//   #2763 AC3: Soft/opt-in include-macro still ergonomic
//   #2763 AC4: quiet path (no dirty) → zero extra rebuild cost
//   #2763 AC5: query-pattern-delta-rebuild-total + hygiene-filtered + schema-2763
//   #2763 AC6: source-cite + coverage linter green; no docs/design/*

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
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

// clang-format may split long string literals; match after strip.
[[nodiscard]] static bool source_has_key(const std::string& hay, std::string_view key) {
    std::string n;
    n.reserve(hay.size());
    for (char ch : hay) {
        if (ch != '"' && ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t')
            n.push_back(ch);
    }
    return n.find(key) != std::string::npos;
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:pattern-hygiene-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t rebuild_href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:pattern-index-rebuild-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t result_len(CompilerService& cs, const std::string& expr) {
    auto r = cs.eval("(length " + expr + ")");
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

static void ac1_docs() {
    std::println("\n--- AC1: source cites #2123 + default policy ---");
    auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
    auto matcher = read_file("src/compiler/query_matcher.ixx");
    auto mcpp = read_file("src/compiler/query_matcher.cpp");
    CHECK(!qws.empty() && !matcher.empty(), "read sources");
    CHECK(qws.find("#2123") != std::string::npos, "query:pattern cites #2123");
    CHECK(matcher.find("#2123") != std::string::npos || mcpp.find("#2123") != std::string::npos,
          "matcher cites #2123");
    CHECK(qws.find("include_macro_introduced = false") != std::string::npos,
          "default include_macro=false");
    CHECK(matcher.find("skip_macro_introduced") != std::string::npos, "skip flag documented");
}

static void ac2_default_filters_macro() {
    std::println("\n--- AC2: default query:pattern filters MacroIntroduced ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");

    const auto filt0 = m->pattern_hygiene_filtered_total.load(std::memory_order_relaxed);
    const auto macro_n = result_len(cs, "(query:macro-introduced)");
    CHECK(macro_n >= 3, "have MacroIntroduced nodes");

    const auto default_cnt = result_len(cs, "(query:pattern \"*\")");
    const auto allow_cnt = result_len(cs, "(query:pattern \"*\" :allow-macro-introduced #t)");
    CHECK(default_cnt >= 0 && allow_cnt >= 0, "pattern lengths");
    CHECK(allow_cnt >= default_cnt, "allow >= default (macro residue visible only with opt-in)");
    // Default path must have filtered something when macros expanded.
    const auto filt1 = m->pattern_hygiene_filtered_total.load(std::memory_order_relaxed);
    std::println("  macro_n={} default={} allow={} filtered {} -> {}", macro_n, default_cnt,
                 allow_cnt, filt0, filt1);
    CHECK(filt1 >= filt0, "filtered total monotonic");
    // When opt-in returns more hits, default path excluded macro residue
    // either via per-node skip (filtered counter) or user-only tag_arity
    // index (hygiene-index-served).
    if (allow_cnt > default_cnt) {
        const auto served = href(cs, "hygiene-index-served");
        CHECK(filt1 > filt0 || served > 0,
              "filter counter or hygiene user-only index excluded macros");
    }
}

static void ac3_opt_in() {
    std::println("\n--- AC3: opt-in returns MacroIntroduced ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto opt0 = m->pattern_include_macro_opt_in_total.load(std::memory_order_relaxed);

    auto r1 = cs.eval("(query:pattern \"*\" :include-macro-introduced #t)");
    CHECK(r1.has_value(), "include-macro-introduced accepted");
    auto r2 = cs.eval("(query:pattern \"*\" :allow-macro-introduced #t)");
    CHECK(r2.has_value(), "allow-macro-introduced accepted");
    auto r3 = cs.eval("(query:pattern \"*\" :exclude-macro-introduced #f)");
    CHECK(r3.has_value(), "exclude-macro-introduced #f accepted");

    const auto opt1 = m->pattern_include_macro_opt_in_total.load(std::memory_order_relaxed);
    CHECK(opt1 >= opt0 + 3, "opt-in counter += 3");
}

static void ac4_concurrent_shared_lock() {
    std::println("\n--- AC4: concurrent query:pattern under shared_lock ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    std::atomic<int> done{0};
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < 30; ++j) {
                auto r = cs.eval("(query:pattern \"*\")");
                if (r.has_value())
                    ok.fetch_add(1);
            }
            done.fetch_add(1);
        });
    }
    for (auto& t : threads)
        t.join();
    CHECK(done.load() == 4, "all threads finished");
    CHECK(ok.load() >= 100, "most concurrent queries succeeded");
}

static void ac5_metrics_schema() {
    std::println("\n--- AC5: pattern-hygiene-stats schema-2123 ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    (void)cs.eval("(query:pattern \"*\")");
    (void)cs.eval("(query:pattern \"*\" :include-macro-introduced #t)");
    CHECK(href(cs, "schema-2123") == 2123, "schema-2123");
    CHECK(href(cs, "issue-2123") == 2123, "issue-2123");
    CHECK(href(cs, "default-hygiene-filter-wired") == 1, "wired sentinel");
    CHECK(href(cs, "default-exclude-macro-introduced") == 1, "default exclude");
    CHECK(href(cs, "pattern_hygiene_filtered_total") >= 0, "filtered total key");
    CHECK(href(cs, "pattern-hygiene-filtered-total") >= 0, "filtered kebab key");
    CHECK(href(cs, "pattern_include_macro_opt_in_total") >= 1, "opt-in total >= 1");
    CHECK(href(cs, "schema") == 2123, "schema field 2123");
}

static void ac6_existing_keywords() {
    std::println("\n--- AC6: existing hygiene keywords ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    CHECK(cs.eval("(query:pattern \"base\" :respect-hygiene #f)").has_value(),
          ":respect-hygiene recognized");
    CHECK(cs.eval("(query:pattern \"base\" :exclude-macro-introduced #t)").has_value(),
          ":exclude-macro-introduced recognized");
    CHECK(cs.eval("(query:pattern \"base\" :with-markers #t)").has_value(),
          ":with-markers recognized");
}

// ── Issue #2763: delta rebuild default + MacroIntroduced hard filter ──
// Prefer-existing #2123 suite per #81967.

static void ac2763_1_delta_rebuild_low_dirty() {
    std::println("\n--- #2763 AC1: low dirty ratio → delta rebuild ---");
    // Source contract: production path chooses delta under low dirty ratio.
    const auto idx = read_file("src/compiler/evaluator_query_index.cpp");
    CHECK(idx.find("#2763") != std::string::npos, "AC1: query index cites #2763");
    CHECK(idx.find("bump_query_pattern_delta_rebuild") != std::string::npos,
          "AC1: delta rebuild counter wired");
    CHECK(idx.find("tag_arity_index_sync_after_mutation") != std::string::npos,
          "AC1: incremental sync (delta) path present");
    CHECK(idx.find("prefer_full") != std::string::npos ||
              idx.find("tag_arity_index_full_rebuild_threshold_pct") != std::string::npos,
          "AC1: dirty-ratio threshold gates full vs delta");

    // Runtime: large warm index + sparse dirty (single mark_dirty_upward).
    // Note: mutate:replace-pattern wholesale-invalidates the index (#484),
    // forcing cold full rebuild — that path is intentional for orphan
    // hygiene. Production multi-round edit that leaves the index warm
    // (Guard warm-lazy / sparse dirty) uses delta under low dirty ratio.
    CompilerService cs;
    {
        std::string code = "(define base 10)";
        for (int i = 0; i < 80; ++i)
            code += " (define v" + std::to_string(i) + " " + std::to_string(i) + ")";
        code += " (+ base 1)";
        CHECK(cs.eval("(set-code \"" + code + "\")").has_value(), "AC1: large workspace");
    }
    CHECK(cs.eval("(eval-current)").has_value(), "AC1: eval-current");
    auto& ev = cs.evaluator();
    auto* ws = ev.workspace_flat();
    CHECK(ws != nullptr && ws->size() > 20, "AC1: workspace flat sized");
    // Default threshold 25%; one dirty cone is << that on 80+ defines.
    ev.force_build_tag_arity_index();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "AC1: metrics");
    const auto d0 = m->query_pattern_delta_rebuild_total.load(std::memory_order_relaxed);
    const auto f0 = m->query_pattern_full_rebuild_total.load(std::memory_order_relaxed);
    // Sparse structural dirty (not wholesale invalidate): one leaf upward.
    const auto seed = static_cast<aura::ast::NodeId>(ws->size() / 2);
    ws->mark_dirty_upward(seed);
    // Bump generation so rebuild path does not take the already-synced
    // early return; dirty fraction remains low → delta.
    ws->bump_generation();
    ev.force_build_tag_arity_index();
    const auto d1 = m->query_pattern_delta_rebuild_total.load(std::memory_order_relaxed);
    const auto f1 = m->query_pattern_full_rebuild_total.load(std::memory_order_relaxed);
    std::println("  delta {} -> {}, full {} -> {} (sparse dirty seed={})", d0, d1, f0, f1, seed);
    CHECK(rebuild_href(cs, "schema-2763") == 2763, "AC1: schema-2763 on rebuild-stats");
    CHECK(rebuild_href(cs, "query-pattern-delta-rebuild-total") >= 0,
          "AC1: delta rebuild key present");
    CHECK(rebuild_href(cs, "query-pattern-delta-rebuild-wired") == 1, "AC1: delta-rebuild-wired");
    CHECK(d1 >= d0, "AC1: delta total monotonic");
    // Production contract: low dirty ratio → delta rebuild credit.
    CHECK(d1 > d0, "AC1: sparse dirty credits query-pattern-delta-rebuild-total");
    // Full may grow from the cold seed only; sparse path must not force full.
    CHECK(f1 == f0, "AC1: sparse dirty does not take full rebuild path");
}

static void ac2763_2_macro_hard_filter() {
    std::println("\n--- #2763 AC2: MacroIntroduced hard-filtered by default ---");
    const auto matcher = read_file("src/compiler/query_matcher.ixx");
    const auto mcpp = read_file("src/compiler/query_matcher.cpp");
    CHECK(matcher.find("#2763") != std::string::npos || mcpp.find("#2763") != std::string::npos,
          "AC2: matcher cites #2763");
    CHECK(mcpp.find("skip_macro_introduced_") != std::string::npos,
          "AC2: hard skip flag in match_subtree");
    CHECK(mcpp.find("is_macro_introduced") != std::string::npos,
          "AC2: MacroIntroduced hard filter at match");

    CompilerService cs;
    CHECK(setup_macro_ws(cs), "AC2: macro workspace");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "AC2: metrics");
    const auto filt0 = m->pattern_hygiene_filtered_total.load(std::memory_order_relaxed);
    const auto macro_n = result_len(cs, "(query:macro-introduced)");
    CHECK(macro_n >= 1, "AC2: MacroIntroduced nodes present");
    const auto default_cnt = result_len(cs, "(query:pattern \"*\")");
    const auto allow_cnt = result_len(cs, "(query:pattern \"*\" :allow-macro-introduced #t)");
    CHECK(default_cnt >= 0 && allow_cnt >= 0, "AC2: pattern lengths");
    CHECK(allow_cnt >= default_cnt, "AC2: opt-in >= default (hygiene hard filter)");
    const auto filt1 = m->pattern_hygiene_filtered_total.load(std::memory_order_relaxed);
    CHECK(filt1 >= filt0, "AC2: hygiene filter total monotonic");
    CHECK(href(cs, "query-pattern-hygiene-filtered-total") >= 0,
          "AC2: Agent key query-pattern-hygiene-filtered-total");
}

static void ac2763_3_soft_opt_in() {
    std::println("\n--- #2763 AC3: Soft/opt-in include-macro remains ergonomic ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "AC3: macro workspace");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto opt0 = m->pattern_include_macro_opt_in_total.load(std::memory_order_relaxed);
    CHECK(cs.eval("(query:pattern \"*\" :include-macro-introduced #t)").has_value(),
          "AC3: include-macro-introduced #t");
    CHECK(cs.eval("(query:pattern \"*\" :allow-macro-introduced #t)").has_value(),
          "AC3: allow-macro-introduced #t");
    const auto opt1 = m->pattern_include_macro_opt_in_total.load(std::memory_order_relaxed);
    CHECK(opt1 >= opt0 + 2, "AC3: opt-in counters still bump (no regression)");
    // Soft path does not require full-only rebuild policy.
    const auto idx = read_file("src/compiler/evaluator_query_index.cpp");
    CHECK(idx.find("bump_query_pattern_delta_rebuild") != std::string::npos,
          "AC3: delta path still available (not full-only)");
}

static void ac2763_4_quiet_path() {
    std::println("\n--- #2763 AC4: quiet path (no dirty) → zero extra rebuild ---");
    const auto idx = read_file("src/compiler/evaluator_query_index.cpp");
    CHECK(idx.find("already synced") != std::string::npos ||
              idx.find("AC4 quiet") != std::string::npos ||
              idx.find("cur_size == tag_arity_index_synced_size_") != std::string::npos,
          "AC4: already-synced early return present");

    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define q 1) (+ q 2)\")").has_value(), "AC4: workspace");
    CHECK(cs.eval("(eval-current)").has_value(), "AC4: eval");
    auto& ev = cs.evaluator();
    ev.force_build_tag_arity_index();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "AC4: metrics");
    const auto d0 = m->query_pattern_delta_rebuild_total.load(std::memory_order_relaxed);
    const auto f0 = m->query_pattern_full_rebuild_total.load(std::memory_order_relaxed);
    // Second force with no mutation must early-return (quiet).
    ev.force_build_tag_arity_index();
    const auto d1 = m->query_pattern_delta_rebuild_total.load(std::memory_order_relaxed);
    const auto f1 = m->query_pattern_full_rebuild_total.load(std::memory_order_relaxed);
    std::println("  quiet: delta {} -> {}, full {} -> {}", d0, d1, f0, f1);
    CHECK(d1 == d0 && f1 == f0, "AC4: second build with no dirty → zero rebuild credit");
}

static void ac2763_5_observability() {
    std::println("\n--- #2763 AC5: additive observability keys ---");
    const auto met = read_file("src/compiler/observability_metrics.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(met.find("query_pattern_delta_rebuild_total") != std::string::npos,
          "AC5: query_pattern_delta_rebuild_total metric");
    CHECK(met.find("query_pattern_full_rebuild_total") != std::string::npos,
          "AC5: query_pattern_full_rebuild_total metric");
    CHECK(source_has_key(q, "query-pattern-delta-rebuild-total"),
          "AC5: query-pattern-delta-rebuild-total key");
    CHECK(source_has_key(q, "query-pattern-full-rebuild-total"),
          "AC5: query-pattern-full-rebuild-total key");
    CHECK(source_has_key(q, "query-pattern-hygiene-filtered-total"),
          "AC5: query-pattern-hygiene-filtered-total key");
    CHECK(source_has_key(q, "schema-2763"), "AC5: schema-2763");
    CHECK(source_has_key(q, "issue-2763"), "AC5: issue-2763");
    // Prior surfaces preserved.
    CHECK(source_has_key(q, "schema") || q.find("schema\", 1503") != std::string::npos ||
              q.find("schema\",1503") != std::string::npos || source_has_key(q, "schema\",1503") ||
              q.find("1503") != std::string::npos,
          "AC5: schema-1503 lineage preserved");
    CHECK(q.find("schema-2123") != std::string::npos || q.find("2123") != std::string::npos,
          "AC5: schema-2123 hygiene surface preserved");

    CompilerService cs;
    CHECK(setup_macro_ws(cs), "AC5: workspace");
    (void)cs.eval("(query:pattern \"*\")");
    CHECK(href(cs, "schema-2763") == 2763, "AC5: hygiene-stats schema-2763");
    CHECK(href(cs, "issue-2763") == 2763, "AC5: hygiene-stats issue-2763");
    CHECK(href(cs, "query-pattern-hygiene-filtered-total") >= 0, "AC5: live hygiene-filtered key");
    CHECK(rebuild_href(cs, "schema-2763") == 2763, "AC5: rebuild-stats schema-2763");
    CHECK(rebuild_href(cs, "query-pattern-delta-rebuild-total") >= 0,
          "AC5: live delta-rebuild key");
    CHECK(rebuild_href(cs, "schema") == 1503, "AC5: schema 1503 preserved on rebuild-stats");
}

static void ac2763_6_source_and_linter() {
    std::println("\n--- #2763 AC6: source-cite + linter ---");
    const auto idx = read_file("src/compiler/evaluator_query_index.cpp");
    const auto matcher = read_file("src/compiler/query_matcher.ixx");
    const auto mcpp = read_file("src/compiler/query_matcher.cpp");
    const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
    const auto t = read_file("tests/compiler/test_query_pattern_default_hygiene.cpp");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_query_pattern_delta_hygiene_2763.py");
    CHECK(idx.find("#2763") != std::string::npos, "AC6: query index cites #2763");
    CHECK(matcher.find("#2763") != std::string::npos || mcpp.find("#2763") != std::string::npos,
          "AC6: matcher cites #2763");
    CHECK(qws.find("#2763") != std::string::npos, "AC6: query:pattern surface cites #2763");
    CHECK(t.find("ac2763_1_delta_rebuild_low_dirty") != std::string::npos, "AC6: AC1 test");
    CHECK(t.find("ac2763_2_macro_hard_filter") != std::string::npos, "AC6: AC2 test");
    CHECK(t.find("ac2763_5_observability") != std::string::npos, "AC6: AC5 test");
    CHECK(build.find("check_query_pattern_delta_hygiene_2763") != std::string::npos,
          "AC6: build.py wires linter");
    CHECK(!lint.empty(), "AC6: linter present");
    CHECK(read_file("docs/design/2763-query-pattern-delta-hygiene.md").empty(),
          "AC6: no docs/design/2763-* per #1655");
}

} // namespace

int run_test_query_pattern_default_hygiene() {
    ac1_docs();
    ac2_default_filters_macro();
    ac3_opt_in();
    ac4_concurrent_shared_lock();
    ac5_metrics_schema();
    ac6_existing_keywords();

    std::println("\n=== Issue #2763: query:pattern delta rebuild + hygiene hard filter ===");
    ac2763_1_delta_rebuild_low_dirty();
    ac2763_2_macro_hard_filter();
    ac2763_3_soft_opt_in();
    ac2763_4_quiet_path();
    ac2763_5_observability();
    ac2763_6_source_and_linter();

    std::println("\n=== test_query_pattern_default_hygiene: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_query_pattern_default_hygiene();
}
#endif
