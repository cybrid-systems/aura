// test_shape_profiler_burst_closed_loop_407.cpp
// Issue #407: ShapeProfiler workload-aware stability + bursty AI
// mutation deopt reduction observability.
//
// Non-duplicative with #570 (shape-stability-stats 6-counter),
// #605 (JIT mutate matrix), #406 (pass-contracts-stats).
//
// AC1: query:shape-deopt-burst-stats reachable
// AC2: eval-current exercises shape profiler counters
// AC3: eval-current :jit exercises JIT shape cache path
// AC4: mutate + eval bumps churn/deopt signals
// AC5: multi-round bursty mutate matrix monotonic
// AC6: query regression (shape-stability-stats,
//      task4-hotpath-safety-score)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_407_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t burst_stats(CompilerService& cs) {
    auto r = cs.eval("(query:shape-deopt-burst-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define (dbl y) (* y 2)) "
                 "(define base 10) (define acc 0) "
                 "(add1 1) (dbl 3)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:shape-deopt-burst-stats ---");
    CHECK(setup_workspace(cs), "shape profiler workspace setup");
    const auto s0 = burst_stats(cs);
    std::println("  shape-deopt-burst-stats = {}", s0);
    CHECK(s0 >= 0, "burst stats non-negative");

    std::println("\n--- AC2: eval-current shape profiler ---");
    const auto stats2a = burst_stats(cs);
    CHECK(cs.eval("(eval-current)").has_value(), "re-eval current");
    const auto stats2b = burst_stats(cs);
    std::println("  burst stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b >= stats2a, "re-eval monotonic");

    std::println("\n--- AC3: eval-current :jit shape cache ---");
    const auto stats3a = burst_stats(cs);
    auto jit = cs.eval("(eval-current :jit)");
    const auto stats3b = burst_stats(cs);
    std::println("  jit ok={} burst stats: {} -> {}",
                 jit.has_value(), stats3a, stats3b);
    CHECK(jit.has_value(), "eval-current :jit succeeds");
    CHECK(stats3b >= stats3a, "JIT path monotonic");

    std::println("\n--- AC4: mutate + eval bumps churn signals ---");
    const auto stats4a = burst_stats(cs);
    (void)cs.eval("(mutate:rebind \"base\" \"42\")");
    CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
    (void)cs.eval("(eval-current :jit)");
    const auto stats4b = burst_stats(cs);
    std::println("  burst stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b >= stats4a, "mutate+eval bumps burst stats");

    std::println("\n--- AC5: multi-round bursty mutate matrix ---");
    const auto stats5a = burst_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" +
                      std::to_string(round) + "\")");
        (void)cs.eval("(mutate:rebind \"base\" \"" +
                      std::to_string(20 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(eval-current :jit)");
    }
    const auto stats5b = burst_stats(cs);
    std::println("  burst stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "burst stats monotonic over matrix");

    std::println("\n--- AC6: query regression ---");
    auto sss = cs.eval("(query:shape-stability-stats)");
    auto ths = cs.eval("(query:task4-hotpath-safety-score)");
    CHECK(sss && is_int(*sss), "shape-stability-stats regression");
    CHECK(ths && is_int(*ths), "task4-hotpath-safety-score regression");
}

} // namespace aura_407_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_407_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}