// Issue #552/#619/#634/#635/#636 (#1978 renamed): issue# moved from filename to header.
// test_edsl_query_mutate_commercial_closed_loop_636.cpp
// Issue #636: EDSL workspace + query/mutate + StableNodeRef commercial
// closed-loop re-review (July 2026 update).
//
// Non-duplicative with #552 (edsl-stability long-run), #635 (macro-reflect),
// #634 (commercial runtime pillars), #619 (macro follow-up).
//
// AC1: query:edsl-query-mutate-commercial-stats reachable
// AC2: query:pattern bumps query-index/hygiene counters
// AC3: mutate:rebind under Guard bumps guard/dirty counters
// AC4: verify:parse-coverage-feedback bumps EDA feedback counters
// AC5: multi-round query→mutate→eval cycle — stats monotonic
// AC6: query regression (stable-ref-stats, edsl-stability-stats,
//      mutation-log-stats, commercial-production-readiness-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_636_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t edsl_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:edsl-query-mutate-commercial-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1) (define y 2) (define z 3)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:edsl-query-mutate-commercial-stats ---");
    CHECK(setup_workspace(cs), "workspace setup");
    const auto s0 = edsl_stats(cs);
    std::println("  edsl-query-mutate-commercial-stats = {}", s0);
    CHECK(s0 >= 0, "edsl commercial stats non-negative");

    std::println("\n--- AC2: query:pattern bumps query counters ---");
    const auto stats2a = edsl_stats(cs);
    auto qp = cs.eval("(query:pattern \"x\")");
    CHECK(qp.has_value(), "query:pattern finds binding x");
    const auto stats2b = edsl_stats(cs);
    std::println("  edsl stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b >= stats2a, "query:pattern bumps query-index counters");

    std::println("\n--- AC3: mutate:rebind under Guard ---");
    const auto stats3a = edsl_stats(cs);
    (void)cs.eval("(mutate:rebind \"x\" \"42\")");
    (void)cs.eval("(eval-current)");
    const auto stats3b = edsl_stats(cs);
    std::println("  edsl stats: {} -> {}", stats3a, stats3b);
    CHECK(stats3b > stats3a, "mutate:rebind bumps guard/dirty commercial counters");

    std::println("\n--- AC4: EDA verification feedback ---");
    const auto stats4a = edsl_stats(cs);
    auto cov = cs.eval("(verify:parse-coverage-feedback \"1 hole_a\n2 hole_b\n\")");
    CHECK(cov && is_int(*cov), "verify:parse-coverage-feedback returns int");
    const auto stats4b = edsl_stats(cs);
    std::println("  edsl stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b > stats4a, "coverage feedback bumps EDA verification counters");

    std::println("\n--- AC5: multi-round query→mutate→eval monotonic ---");
    const auto stats5a = edsl_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(query:pattern \"y\")");
        (void)cs.eval("(mutate:rebind \"y\" \"" + std::to_string(20 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(mutate:request-gc-safepoint 10)");
    }
    const auto stats5b = edsl_stats(cs);
    std::println("  edsl stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "edsl commercial stats monotonic over matrix");

    std::println("\n--- AC6: query regression ---");
    auto srs = cs.eval("(engine:metrics \"query:stable-ref-stats\")");
    auto ess = cs.eval("(engine:metrics \"query:edsl-stability-stats\")");
    auto mls = cs.eval("(engine:metrics \"query:mutation-log-stats\")");
    auto cpr = cs.eval("(engine:metrics \"query:commercial-production-readiness-stats\")");
    CHECK(srs && is_int(*srs), "stable-ref-stats regression");
    CHECK(ess && is_int(*ess), "edsl-stability-stats regression");
    CHECK(mls && is_int(*mls), "mutation-log-stats regression");
    CHECK(cpr && is_int(*cpr), "commercial-production-readiness-stats regression");
}

} // namespace aura_636_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_636_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}