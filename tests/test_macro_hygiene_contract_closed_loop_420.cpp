// test_macro_hygiene_contract_closed_loop_420.cpp
// Issue #420: Post P1/P2 split end-to-end MacroIntroduced
// hygiene contract across clone/expand/query/mutate/IR.
//
// Non-duplicative with #458 (hygiene-stats skip-only),
// #547 (pattern-hygiene-stats 2-counter), #514
// (ir-hygiene-stats / pattern-marker-stats slices).
//
// AC1: query:macro-hygiene-contract-stats reachable
// AC2: hygienic macro eval bumps marker + macro_dirty counters
// AC3: query:pattern default hygiene filter bumps query_skips
// AC4: query:macro-introduced matches query:marker-stats macro
// AC5: ensure_macro_hygiene_contract — zero violations
// AC6: multi-round query:pattern matrix monotonic
// AC7: query regression (pattern-hygiene-stats, ir-hygiene-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_420_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;

static std::int64_t macro_hygiene_contract_stats(CompilerService& cs) {
    auto r = cs.eval("(query:macro-hygiene-contract-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_macro_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (d y) (* y 2)) "
                 "(d 1) (d 2) (d 3)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:macro-hygiene-contract-stats ---");
    CHECK(setup_macro_workspace(cs), "macro hygiene workspace setup");
    const auto s0 = macro_hygiene_contract_stats(cs);
    std::println("  macro-hygiene-contract-stats = {}", s0);
    CHECK(s0 > 0, "macro hygiene contract stats positive after macro eval");

    std::println("\n--- AC2: marker + macro_dirty counters ---");
    auto& ev = cs.evaluator();
    auto* ws = ev.workspace_flat();
    CHECK(ws != nullptr, "workspace flat available");
    const auto markers = cs.eval("(length (query:macro-introduced))");
    CHECK(markers && is_int(*markers), "macro-introduced returns int");
    std::println("  macro-introduced count = {}", as_int(*markers));
    CHECK(as_int(*markers) >= 3, "macro-introduced >= 3 nodes");
    CHECK(ws->macro_expansion_dirty_total() > 0,
          "macro_expansion_dirty_total bumped by clone/expand");

    std::println("\n--- AC3: query:pattern hygiene filter ---");
    const auto stats3a = macro_hygiene_contract_stats(cs);
    (void)cs.eval("(query:pattern \"*\")");
    const auto stats3b = macro_hygiene_contract_stats(cs);
    const auto skips3 = ev.get_macro_introduced_skipped_in_query();
    std::println("  contract stats: {} -> {}", stats3a, stats3b);
    std::println("  query_skips = {}", skips3);
    CHECK(stats3b > stats3a, "query:pattern bumps contract stats");
    CHECK(skips3 > 0, "query:pattern bumps macro_introduced_skipped");

    std::println("\n--- AC4: macro-introduced vs by-marker ---");
    auto macro_from_query = cs.eval("(length (query:macro-introduced))");
    auto macro_from_marker = cs.eval("(length (query:by-marker \"MacroIntroduced\"))");
    CHECK(macro_from_query && is_int(*macro_from_query), "macro-introduced length is int");
    CHECK(macro_from_marker && is_int(*macro_from_marker),
          "by-marker MacroIntroduced length is int");
    std::println("  macro-introduced = {}, by-marker = {}", as_int(*macro_from_query),
                 as_int(*macro_from_marker));
    CHECK(as_int(*macro_from_query) == as_int(*macro_from_marker),
          "macro-introduced matches by-marker MacroIntroduced");

    std::println("\n--- AC5: ensure_macro_hygiene_contract happy path ---");
    ev.ensure_macro_hygiene_contract();
    CHECK(ev.get_macro_hygiene_contract_violations() == 0,
          "zero macro hygiene contract violations");

    std::println("\n--- AC6: multi-round query:pattern matrix ---");
    const auto stats6a = macro_hygiene_contract_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(query:pattern \"*\")");
        (void)cs.eval("(query:macro-introduced)");
    }
    const auto stats6b = macro_hygiene_contract_stats(cs);
    std::println("  contract stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b > stats6a, "contract stats grow over query matrix");

    std::println("\n--- AC7: query regression ---");
    auto phs = cs.eval("(query:pattern-hygiene-stats)");
    auto ihs = cs.eval("(query:ir-hygiene-stats)");
    CHECK(phs && is_int(*phs), "pattern-hygiene-stats regression");
    CHECK(ihs && is_int(*ihs), "ir-hygiene-stats regression");
}

} // namespace aura_420_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_420_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}