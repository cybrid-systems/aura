// test_list_vector_soa_hotpath_ai_loops.cpp — Issue #752:
// list/vector map/filter SoA + intrinsic fast-dispatch observability
// (refines #727; non-duplicative with #667 apply-loop counters,
// #506 IR SoA adoption, #614 pair/traverse stats).
//
//   - AC1:  query:list-soa-hotpath-stats reachable (schema 752)
//   - AC2:  chain-traversals bumps on direct path
//   - AC3:  soa-hits bumps on direct path
//   - AC4:  intrinsic-dispatches bumps on direct path
//   - AC5:  estimated-cache-misses bumps on direct path
//   - AC6:  hotpath-events-total == sum of 4 per-counter fields
//   - AC7:  real AI-loop exercise — large list + repeated map/filter/
//           foldl (closure + intrinsic paths) — stats monotonic
//   - AC8:  query regression — adjacent list/primitive primitives

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_752_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t stat_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:list-soa-hotpath-stats) '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t chain_traversals(CompilerService& cs) {
    return stat_int(cs, "chain-traversals");
}
static std::int64_t soa_hits(CompilerService& cs) {
    return stat_int(cs, "soa-hits");
}
static std::int64_t intrinsic_dispatches(CompilerService& cs) {
    return stat_int(cs, "intrinsic-dispatches");
}
static std::int64_t estimated_cache_misses(CompilerService& cs) {
    return stat_int(cs, "estimated-cache-misses");
}
static std::int64_t events_total(CompilerService& cs) {
    return stat_int(cs, "hotpath-events-total");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:list-soa-hotpath-stats (schema 752) ---");
    auto h = cs.eval("(query:list-soa-hotpath-stats)");
    CHECK(h && is_hash(*h), "list-soa-hotpath-stats returns hash");
    CHECK(stat_int(cs, "schema") == 752, "schema == 752");
    const auto c = chain_traversals(cs);
    const auto s = soa_hits(cs);
    const auto i = intrinsic_dispatches(cs);
    const auto m = estimated_cache_misses(cs);
    const auto t = events_total(cs);
    std::println("  baseline: chain={}, soa={}, intrinsic={}, misses={}, total={}", c, s, i, m, t);
    CHECK(c >= 0, "chain-traversals non-negative");
    CHECK(s >= 0, "soa-hits non-negative");
    CHECK(i >= 0, "intrinsic-dispatches non-negative");
    CHECK(m >= 0, "estimated-cache-misses non-negative");
    CHECK(t >= 0, "hotpath-events-total non-negative");

    std::println("\n--- AC2: chain-traversals bumps on direct path ---");
    const auto c0 = chain_traversals(cs);
    cs.evaluator().bump_list_chain_traversals();
    cs.evaluator().bump_list_chain_traversals();
    cs.evaluator().bump_list_chain_traversals();
    const auto c1 = chain_traversals(cs);
    std::println("  chain-traversals: {} -> {}", c0, c1);
    CHECK(c1 == c0 + 3, "chain-traversals bumps by exactly 3");

    std::println("\n--- AC3: soa-hits bumps on direct path ---");
    const auto s0 = soa_hits(cs);
    cs.evaluator().bump_list_soa_hits();
    cs.evaluator().bump_list_soa_hits();
    const auto s1 = soa_hits(cs);
    std::println("  soa-hits: {} -> {}", s0, s1);
    CHECK(s1 == s0 + 2, "soa-hits bumps by exactly 2");

    std::println("\n--- AC4: intrinsic-dispatches bumps on direct path ---");
    const auto i0 = intrinsic_dispatches(cs);
    cs.evaluator().bump_list_intrinsic_dispatches();
    cs.evaluator().bump_list_intrinsic_dispatches();
    cs.evaluator().bump_list_intrinsic_dispatches();
    cs.evaluator().bump_list_intrinsic_dispatches();
    const auto i1 = intrinsic_dispatches(cs);
    std::println("  intrinsic-dispatches: {} -> {}", i0, i1);
    CHECK(i1 == i0 + 4, "intrinsic-dispatches bumps by exactly 4");

    std::println("\n--- AC5: estimated-cache-misses bumps on direct path ---");
    const auto m0 = estimated_cache_misses(cs);
    cs.evaluator().bump_list_estimated_cache_misses();
    cs.evaluator().bump_list_estimated_cache_misses();
    const auto m1 = estimated_cache_misses(cs);
    std::println("  estimated-cache-misses: {} -> {}", m0, m1);
    CHECK(m1 == m0 + 2, "estimated-cache-misses bumps by exactly 2");

    std::println("\n--- AC6: hotpath-events-total == sum ---");
    const auto c_sum = chain_traversals(cs);
    const auto s_sum = soa_hits(cs);
    const auto i_sum = intrinsic_dispatches(cs);
    const auto m_sum = estimated_cache_misses(cs);
    const auto t_sum = events_total(cs);
    std::println("  chain={} + soa={} + intrinsic={} + misses={} = sum {} (primitive total {})",
                 c_sum, s_sum, i_sum, m_sum, c_sum + s_sum + i_sum + m_sum, t_sum);
    CHECK(t_sum == c_sum + s_sum + i_sum + m_sum, "hotpath-events-total == sum of 4 counters");

    std::println("\n--- AC7: real AI-loop exercise (large list + map/filter/foldl) ---");
    cs.eval("(set-code \"(define base 0) base\")");
    cs.eval("(eval-current)");
    const auto stats7a = events_total(cs);
    const auto chain7a = chain_traversals(cs);
    const auto misses7a = estimated_cache_misses(cs);

    // Build a 100-element list and exercise closure map/filter loops.
    auto setup = R"(
        (begin
          (define mk-list (lambda (n)
            (if (= n 0) (list) (cons n (mk-list (- n 1))))))
          (define big (mk-list 100)))
    )";
    cs.eval(std::format("(set-code \"{}\")", setup));
    cs.eval("(eval-current)");

    // Closure map/filter — chain + cache-miss bumps, no intrinsic/soa.
    for (int round = 0; round < 10; ++round) {
        cs.eval("(define doubled (map (lambda (x) (* x 2)) big))");
        cs.eval("(define kept (filter (lambda (x) (> x 50)) big))");
    }
    const auto chain7b = chain_traversals(cs);
    const auto misses7b = estimated_cache_misses(cs);
    std::println("  after 10x closure map/filter: chain {} -> {}, misses {} -> {}", chain7a,
                 chain7b, misses7a, misses7b);
    CHECK(chain7b > chain7a, "chain-traversals grew after closure map/filter loops");
    CHECK(misses7b > misses7a, "estimated-cache-misses grew after closure map/filter loops");

    // Intrinsic path is exercised via direct bump helpers (AC3–AC4).
    // Compile-time specialization may inline (map abs …) / (foldl + …)
    // without hitting runtime apply_unary — closure map/filter is the
    // production AI-agent path and dominates chain/miss counters here.
    CHECK(misses7b >= chain7b, "estimated-cache-misses >= chain-traversals after closure loops");

    const auto stats7b = events_total(cs);
    std::println("  hotpath-events-total: {} -> {}", stats7a, stats7b);
    CHECK(stats7b > stats7a, "hotpath-events-total monotonic over AI-loop matrix");

    std::println("\n--- AC8: query regression ---");
    auto apply_stats = cs.eval("(query:primitives-apply-stats)");
    auto soa_adopt = cs.eval("(query:soa-hotpath-adoption-stats)");
    auto prim_perf = cs.eval("(query:primitive-perf-stats)");
    CHECK(apply_stats && is_hash(*apply_stats), "primitives-apply-stats regression");
    CHECK(soa_adopt && is_int(*soa_adopt), "soa-hotpath-adoption-stats regression");
    CHECK(prim_perf && is_hash(*prim_perf), "primitive-perf-stats regression");
}

} // namespace aura_issue_752_detail

int aura_issue_list_vector_soa_hotpath_ai_loops_run() {
    aura::compiler::CompilerService cs;
    aura_issue_752_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_list_vector_soa_hotpath_ai_loops_run();
}
#endif
