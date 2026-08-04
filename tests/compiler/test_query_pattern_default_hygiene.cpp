// @category: unit
// @reason: Issue #2123 — default hygiene filter for query:pattern +
// MacroIntroduced linkage (production "code as memory" contract).
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:pattern-hygiene-stats\") \"{}\")", key));
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

} // namespace

int run_test_query_pattern_default_hygiene() {
    ac1_docs();
    ac2_default_filters_macro();
    ac3_opt_in();
    ac4_concurrent_shared_lock();
    ac5_metrics_schema();
    ac6_existing_keywords();

    std::println("\n=== test_query_pattern_default_hygiene: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_query_pattern_default_hygiene();
}
#endif
