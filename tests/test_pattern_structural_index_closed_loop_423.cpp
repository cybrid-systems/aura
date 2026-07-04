// test_pattern_structural_index_closed_loop_423.cpp
// Issue #423: query:pattern structural pre-indexing for
// large AST performance (Evaluator-side tag_arity_index).
//
// Non-duplicative with #547/#554 (query:pattern-index-stats
// FlatAST workspace slice), #421 (macro filter bundle),
// #211 (index fast-path regression).
//
// AC1: query:pattern-structural-index-stats reachable
// AC2: large AST workspace establishes index baseline
// AC3: query:pattern bumps structural index hits
// AC4: index buckets/entries grow after build
// AC5: ensure_pattern_index_consistency — zero violations
// AC6: multi-round pattern queries monotonic hits
// AC7: query regression (pattern-index-stats,
//      pattern-macro-filter-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_423_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t pattern_structural_index_stats(CompilerService& cs) {
    auto r = cs.eval("(query:pattern-structural-index-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_large_ast_workspace(CompilerService& cs) {
    std::string code = "(define root 0)";
    for (int i = 0; i < 200; ++i) {
        code += " (define v" + std::to_string(i) + " " + std::to_string(i) + ")";
    }
    if (!cs.eval("(set-code \"" + code + "\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:pattern-structural-index-stats ---");
    const auto s0 = pattern_structural_index_stats(cs);
    std::println("  pattern-structural-index-stats = {}", s0);
    CHECK(s0 >= 0, "structural index stats non-negative");

    std::println("\n--- AC2: large AST workspace baseline ---");
    CHECK(setup_large_ast_workspace(cs), "large AST workspace setup");
    auto& ev = cs.evaluator();
    auto* ws = ev.workspace_flat();
    CHECK(ws != nullptr, "workspace flat available");
    std::println("  flat size = {}", ws->size());

    std::println("\n--- AC3: query:pattern bumps structural hits ---");
    const auto stats3a = pattern_structural_index_stats(cs);
    const auto hits3a = ev.get_pattern_structural_index_hits();
    const auto misses3a = ev.get_pattern_structural_index_misses();
    // Non-matching (tag, arity) uses the index miss fast path.
    (void)cs.eval("(query:pattern \"(+ 1 2)\")");
    const auto misses3b = ev.get_pattern_structural_index_misses();
    CHECK(misses3b > misses3a, "non-matching pattern bumps structural index misses");
    // Matching define uses the index hit fast path (strict arity).
    (void)cs.eval("(query:pattern \"(define v0 0)\" :strict-arity #t)");
    const auto stats3b = pattern_structural_index_stats(cs);
    const auto hits3b = ev.get_pattern_structural_index_hits();
    const auto misses3c = ev.get_pattern_structural_index_misses();
    const auto fast3a = hits3a + misses3a;
    const auto fast3b = hits3b + misses3c;
    std::println("  structural stats: {} -> {}", stats3a, stats3b);
    std::println("  fast-path: {} -> {} (hits {} -> {}, misses {} -> {})", fast3a, fast3b, hits3a,
                 hits3b, misses3a, misses3c);
    CHECK(fast3b > fast3a, "query:pattern bumps structural index fast-path");
    CHECK(hits3b > hits3a, "matching define bumps structural index hits");
    CHECK(stats3b > stats3a, "structural index stats grow");

    std::println("\n--- AC4: index buckets/entries after build ---");
    ev.force_build_tag_arity_index();
    const auto buckets = ev.tag_arity_index_size();
    const auto entries = ev.tag_arity_index_entry_count();
    const auto synced = ev.tag_arity_index_synced_size();
    std::println("  buckets = {}, entries = {}, synced_size = {}", buckets, entries, synced);
    CHECK(buckets > 0, "index has buckets after build");
    CHECK(entries > 0, "index has entries after build");
    CHECK(synced == ws->size(), "synced_size matches flat size");

    std::println("\n--- AC5: ensure_pattern_index_consistency ---");
    ev.ensure_pattern_index_consistency(*ws);
    CHECK(ev.get_pattern_index_consistency_violations() == 0,
          "zero pattern index consistency violations");

    std::println("\n--- AC6: multi-round pattern queries monotonic ---");
    const auto fast6a =
        ev.get_pattern_structural_index_hits() + ev.get_pattern_structural_index_misses();
    const auto stats6a = pattern_structural_index_stats(cs);
    for (int round = 0; round < 5; ++round) {
        (void)cs.eval("(query:pattern \"(define v0 0)\" :strict-arity #t)");
    }
    const auto fast6b =
        ev.get_pattern_structural_index_hits() + ev.get_pattern_structural_index_misses();
    const auto stats6b = pattern_structural_index_stats(cs);
    std::println("  fast-path: {} -> {}", fast6a, fast6b);
    CHECK(fast6b > fast6a, "fast-path grows over repeated pattern queries");
    CHECK(stats6b > stats6a, "structural stats grow over matrix");

    std::println("\n--- AC7: query regression ---");
    auto pis = cs.eval("(query:pattern-index-stats)");
    auto pmf = cs.eval("(query:pattern-macro-filter-stats)");
    CHECK(pis && is_int(*pis), "pattern-index-stats regression");
    CHECK(pmf && is_int(*pmf), "pattern-macro-filter-stats regression");
}

} // namespace aura_423_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_423_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}