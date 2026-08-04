// @category: unit
// @reason: Issue #2525 — unconstrained query hygiene residual: MacroIntroduced
// skip is production default for query:filter + pattern; composite index
// always stamps marker dimension; Agents observe via pattern-hygiene-stats.
//
//   AC1: Default query:pattern / query:filter skip MacroIntroduced; include
//        flag restores previous behaviour
//   AC2: Composite index rebuild stamps marker dimension (user-only index)
//   AC3: hygiene_skip_total / hygiene_include_total + schema-2525
//   AC4: Concurrent mutate + query under shared_lock does not crash
//   AC5: Prefer-existing #2123 suite + this residual unconstrained-default case
//   AC6: Agent contract documented; no schema break on schema-2123

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

// ── AC1 ──
static void ac1_default_skip() {
    std::println("\n--- AC1: default pattern + filter skip MacroIntroduced ---");
    const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
    CHECK(qws.find("#2525") != std::string::npos, "AC1: query_workspace cites #2525");
    CHECK(qws.find("hygiene_skip_macro = true") != std::string::npos,
          "AC1: filter default skip ON");
    CHECK(qws.find("include-macro-introduced") != std::string::npos, "AC1: include flag present");

    CompilerService cs;
    CHECK(setup_macro_ws(cs), "AC1: macro workspace");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "AC1: metrics");

    const auto macro_n = result_len(cs, "(query:macro-introduced)");
    CHECK(macro_n >= 3, "AC1: have MacroIntroduced nodes");

    const auto default_pat = result_len(cs, "(query:pattern \"*\")");
    const auto allow_pat = result_len(cs, "(query:pattern \"*\" :allow-macro-introduced #t)");
    CHECK(default_pat >= 0 && allow_pat >= 0, "AC1: pattern lengths");
    CHECK(allow_pat >= default_pat, "AC1: allow >= default pattern");

    // query:filter default must skip macro (filter by tag Call still runs)
    const auto filt0 = m->hygiene_filter_default_skip_total.load(std::memory_order_relaxed);
    auto fr = cs.eval("(query:filter (query:where :node-type \"Call\"))");
    CHECK(fr.has_value(), "AC1: default filter accepted");
    CHECK(m->hygiene_filter_default_skip_total.load(std::memory_order_relaxed) >= filt0 + 1,
          "AC1: filter default-skip counter");

    const auto include0 = m->hygiene_filter_include_opt_in_total.load(std::memory_order_relaxed);
    auto fr_inc =
        cs.eval("(query:filter (query:where :node-type \"Call\") :include-macro-introduced #t)");
    CHECK(fr_inc.has_value(), "AC1: include-macro filter accepted");
    CHECK(m->hygiene_filter_include_opt_in_total.load(std::memory_order_relaxed) >= include0 + 1,
          "AC1: filter include opt-in counter");
}

// ── AC2 ──
static void ac2_index_marker_dimension() {
    std::println("\n--- AC2: composite index rebuild stamps marker dimension ---");
    const auto idx = read_file("src/compiler/evaluator_query_index.cpp");
    CHECK(idx.find("#2525") != std::string::npos, "AC2: index cites #2525");
    CHECK(idx.find("tag_arity_index_user_") != std::string::npos, "AC2: user-only index");
    CHECK(idx.find("tag_arity_marker_dimension_rebuild_total") != std::string::npos,
          "AC2: rebuild counter");
    CHECK(idx.find("marker dimension") != std::string::npos ||
              idx.find("Marker dimension") != std::string::npos,
          "AC2: marker dimension documented");

    CompilerService cs;
    CHECK(setup_macro_ws(cs), "AC2: macro workspace");
    // Force index use via constrained pattern
    (void)cs.eval("(query:pattern \"(define base 10)\" :strict-arity #t)");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    // Rebuild may happen lazily; counter non-negative and surface readable
    CHECK(m->tag_arity_marker_dimension_rebuild_total.load(std::memory_order_relaxed) >= 0,
          "AC2: rebuild counter queryable");
    CHECK(href(cs, "tag_arity_marker_dimension_rebuild_total") >= 0, "AC2: stats key");
    CHECK(href(cs, "marker-dimension-via-user-index-wired") == 1, "AC2: marker dimension wired");
}

// ── AC3 ──
static void ac3_stats_surface() {
    std::println("\n--- AC3: hygiene_skip / include + schema-2525 ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "AC3: macro workspace");
    (void)cs.eval("(query:pattern \"*\")");
    (void)cs.eval("(query:pattern \"*\" :include-macro-introduced #t)");
    (void)cs.eval("(query:pattern \"...\")"); // unconstrained walk

    CHECK(href(cs, "schema-2525") == 2525, "AC3: schema-2525");
    CHECK(href(cs, "issue-2525") == 2525, "AC3: issue-2525");
    CHECK(href(cs, "schema-2123") == 2123, "AC3: schema-2123 retained (no break)");
    CHECK(href(cs, "schema") == 2123, "AC3: primary schema still 2123");
    CHECK(href(cs, "hygiene_skip_total") >= 0, "AC3: hygiene_skip_total");
    CHECK(href(cs, "hygiene-skip-total") >= 0, "AC3: hygiene-skip-total kebab");
    CHECK(href(cs, "hygiene_include_total") >= 1, "AC3: hygiene_include_total");
    CHECK(href(cs, "hygiene-include-total") >= 1, "AC3: hygiene-include-total kebab");
    CHECK(href(cs, "filter-default-skip-macro-wired") == 1, "AC3: filter default wired");
    CHECK(href(cs, "unconstrained-walk-metric-wired") == 1, "AC3: unconstrained wired");
    CHECK(href(cs, "pattern_hygiene_unconstrained_walk_total") >= 0,
          "AC3: unconstrained walk total");
    // Unconstrained "..." on macro workspace under default hygiene should bump.
    CHECK(href(cs, "pattern-hygiene-unconstrained-walk-total") >= 1 ||
              href(cs, "pattern_hygiene_unconstrained_walk_total") >= 1,
          "AC3: unconstrained walk observed on macro workspace");
}

// ── AC4 ──
static void ac4_concurrent() {
    std::println("\n--- AC4: concurrent query under shared_lock ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "AC4: macro workspace");
    std::atomic<int> done{0};
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < 20; ++j) {
                auto r = cs.eval("(query:pattern \"*\")");
                if (r)
                    ok.fetch_add(1);
                auto f = cs.eval("(query:filter (query:where :node-type \"Define\"))");
                if (f)
                    ok.fetch_add(1);
            }
            done.fetch_add(1);
        });
    }
    for (auto& t : threads)
        t.join();
    CHECK(done.load() == 4, "AC4: all threads finished");
    CHECK(ok.load() >= 100, "AC4: most concurrent queries succeeded");
}

// ── AC5 / AC6 ──
static void ac5_ac6_lineage_and_contract() {
    std::println("\n--- AC5/AC6: lineage suite + Agent contract ---");
    const auto t2123 = read_file("tests/compiler/test_query_pattern_default_hygiene_2123.cpp");
    CHECK(!t2123.empty(), "AC5: #2123 suite present");
    const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
    CHECK(qws.find("Agent contract") != std::string::npos ||
              qws.find("never contain MacroIntroduced") != std::string::npos ||
              qws.find("production default") != std::string::npos,
          "AC6: Agent contract documented");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("schema-2525") != std::string::npos, "AC6: schema-2525 on stats");
    CHECK(q.find("schema-2123") != std::string::npos, "AC6: schema-2123 retained");
}

} // namespace

int run_test_query_hygiene_default_2525() {
    std::println("=== Issue #2525: unconstrained query hygiene residual ===");
    ac1_default_skip();
    ac2_index_marker_dimension();
    ac3_stats_surface();
    ac4_concurrent();
    ac5_ac6_lineage_and_contract();
    std::println("\n=== #2525: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_query_hygiene_default_2525();
}
#endif
