// Issue #421 (#1978 renamed): issue# moved from filename to header.
// test_pattern_macro_filter_closed_loop_421.cpp
// Issue #421: query:pattern recursive MacroIntroduced
// filtering after query-primitive split.
//
// Non-duplicative with #547 (pattern-hygiene-stats root skips),
// #420 (macro-hygiene-contract-stats end-to-end bundle).
//
// AC1: query:pattern-macro-filter-stats reachable
// AC2: hygienic macro eval establishes marker baseline
// AC3: query:pattern default hygiene bumps root + recursive skips
// AC4: query:macro-introduced matches query:by-marker
// AC5: ensure_pattern_macro_filter_consistency — zero violations
// AC6: :include-macro-introduced #t includes macro nodes
// AC7: query regression (pattern-hygiene-stats, macro-hygiene-contract-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_421_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t pattern_macro_filter_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:pattern-macro-filter-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_macro_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (d y) (* y 2)) "
                 "(d 1) (d 2) (d 3) "
                 "(define base 10) (+ base 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static std::int64_t result_count(CompilerService& cs, const std::string& expr) {
    auto r = cs.eval("(length " + expr + ")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:pattern-macro-filter-stats ---");
    CHECK(setup_macro_workspace(cs), "pattern macro filter workspace setup");
    const auto s0 = pattern_macro_filter_stats(cs);
    std::println("  pattern-macro-filter-stats = {}", s0);
    CHECK(s0 > 0, "pattern macro filter stats positive after macro eval");

    std::println("\n--- AC2: marker baseline ---");
    auto macro_n = cs.eval("(length (query:macro-introduced))");
    CHECK(macro_n && is_int(*macro_n), "macro-introduced count available");
    CHECK(as_int(*macro_n) >= 3, "macro-introduced >= 3 nodes");

    std::println("\n--- AC3: default query:pattern hygiene filter ---");
    auto& ev = cs.evaluator();
    const auto stats3a = pattern_macro_filter_stats(cs);
    const auto root3a = ev.get_macro_introduced_skipped_in_query();
    const auto rec3a = ev.get_pattern_recursive_macro_skipped();
    (void)cs.eval("(query:pattern \"*\")");
    const auto stats3b = pattern_macro_filter_stats(cs);
    const auto root3b = ev.get_macro_introduced_skipped_in_query();
    const auto rec3b = ev.get_pattern_recursive_macro_skipped();
    std::println("  filter stats: {} -> {}", stats3a, stats3b);
    std::println("  root_skips: {} -> {}, recursive: {} -> {}", root3a, root3b, rec3a, rec3b);
    CHECK(stats3b > stats3a, "query:pattern bumps filter stats");
    CHECK(root3b > root3a, "query:pattern bumps root macro skips");

    std::println("\n--- AC4: macro-introduced vs by-marker ---");
    auto by_marker = cs.eval("(length (query:by-marker \"MacroIntroduced\"))");
    CHECK(by_marker && is_int(*by_marker), "by-marker length is int");
    std::println("  macro-introduced = {}, by-marker = {}", as_int(*macro_n), as_int(*by_marker));
    CHECK(as_int(*macro_n) == as_int(*by_marker), "macro-introduced matches by-marker");

    std::println("\n--- AC5: ensure_pattern_macro_filter_consistency ---");
    auto* ws = ev.workspace_flat();
    CHECK(ws != nullptr, "workspace flat available");
    ev.ensure_pattern_macro_filter_consistency(*ws);
    CHECK(ev.get_pattern_macro_filter_violations() == 0, "zero pattern macro filter violations");

    std::println("\n--- AC6: :include-macro-introduced opt-in ---");
    const auto default_cnt = result_count(cs, "(query:pattern \"*\")");
    const auto include_cnt = result_count(cs, "(query:pattern \"*\" :include-macro-introduced #t)");
    std::println("  default matches = {}, include matches = {}", default_cnt, include_cnt);
    CHECK(default_cnt >= 0 && include_cnt >= 0, "pattern match counts observable");
    CHECK(include_cnt >= default_cnt, "include-macro-introduced yields >= default matches");

    std::println("\n--- AC7: query regression ---");
    auto phs = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    auto mhc = cs.eval("(engine:metrics \"query:macro-hygiene-contract-stats\")");
    CHECK(phs && (is_int(*phs) || is_hash(*phs)), "pattern-hygiene-stats regression");
    CHECK(mhc && is_int(*mhc), "macro-hygiene-contract-stats regression");
}

} // namespace aura_421_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_421_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}