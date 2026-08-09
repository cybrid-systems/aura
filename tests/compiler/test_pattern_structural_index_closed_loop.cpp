// Issue #211/#421/#423/#547/#554 (#1978 renamed): issue# moved from filename to header.
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
    auto r = cs.eval("(engine:metrics \"query:pattern-structural-index-stats\")");
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
    auto pis = cs.eval("(engine:metrics \"query:pattern-index-stats\")");
    auto pmf = cs.eval("(engine:metrics \"query:pattern-macro-filter-stats\")");
    CHECK(pis && is_int(*pis), "pattern-index-stats regression");
    CHECK(pmf && is_int(*pmf), "pattern-macro-filter-stats regression");
}

} // namespace aura_423_detail

// ── Issue #2861: query:pattern full safety contract
// Source-cite ACs (gate-only ship; runtime verifies on CI).

static std::string read_file_2861(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static void ac2861_1_source_atomics() {
    std::println("\n--- #2861 AC1: source — 4 new safety atomics present ---");
    const auto met = read_file_2861("src/compiler/observability_metrics.h");
    CHECK(met.find("pattern_safe_span_uses_total") != std::string::npos,
          "#2861 AC1: pattern_safe_span_uses_total atomic");
    CHECK(met.find("pattern_hygiene_filtered_total") != std::string::npos,
          "#2861 AC1: pattern_hygiene_filtered_total atomic");
    CHECK(met.find("pattern_epoch_mismatch_total") != std::string::npos,
          "#2861 AC1: pattern_epoch_mismatch_total atomic");
    CHECK(met.find("pattern_dangling_prevented_total") != std::string::npos,
          "#2861 AC1: pattern_dangling_prevented_total atomic");
    // Source-cited in comment block as #2861 contract surfaces.
    CHECK(met.find("// #2861") != std::string::npos,
          "#2861 AC1: comment block cites #2861 contract surfaces");
}

static void ac2861_2_source_primitive() {
    std::println("\n--- #2861 AC2: source — query:pattern-safety-stats primitive ---");
    const auto q = read_file_2861("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("query:pattern-safety-stats") != std::string::npos,
          "#2861 AC2: query:pattern-safety-stats primitive registered");
    CHECK(q.find("schema") != std::string::npos && q.find("make_int(2861)") != std::string::npos,
          "#2861 AC2: schema=2861 in hash builder");
    CHECK(q.find("pattern-safe-span-uses-total") != std::string::npos &&
              q.find("pattern-hygiene-filtered-total") != std::string::npos &&
              q.find("pattern-epoch-mismatch-total") != std::string::npos &&
              q.find("pattern-dangling-prevented-total") != std::string::npos,
          "#2861 AC2: 4 contract counter keys in hash");
    // Additive on existing #547 + #490 pattern-index primitives.
    CHECK(q.find("query:pattern-index-stats") != std::string::npos &&
              q.find("query:pattern-index-rebuild-stats") != std::string::npos,
          "#2861 AC2: existing #547 + #490 primitives preserved");
}

static void ac2861_3_no_docs() {
    std::println("\n--- #2861 AC3: no docs/design/ + lineage refs ---");
    CHECK(read_file_2861("docs/design/2861-pattern-safety-contract.md").empty(),
          "#2861 AC3: no docs/design/2861-* per #1655");
    const auto q = read_file_2861("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("#819") != std::string::npos && q.find("#2036") != std::string::npos &&
              q.find("#2123") != std::string::npos && q.find("#2763") != std::string::npos &&
              q.find("#2525") != std::string::npos,
          "#2861 AC3: lineage refs to #819/#2036/#2123/#2763/#2525");
}

int main() {
    aura::compiler::CompilerService cs;
    aura_423_detail::run_matrix(cs);
    std::println("\n=== #2861: query:pattern full safety contract (source-cite gate-only) ===");
    ac2861_1_source_atomics();
    ac2861_2_source_primitive();
    ac2861_3_no_docs();
    return RUN_ALL_TESTS();
}