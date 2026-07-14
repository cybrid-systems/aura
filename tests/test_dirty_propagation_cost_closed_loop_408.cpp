// test_dirty_propagation_cost_closed_loop_408.cpp
// Issue #408: EDSL dirty propagation + per-block dirty_ cost
// observability under high-frequency structural mutation.
//
// Non-duplicative with #415 (dirty-reason-propagation-stats),
// #399 (mark_dirty resize), #398 (children_stable alloc).
//
// AC1: query:dirty-propagation-cost-stats reachable
// AC2: mutate:rebind bumps dirty propagation counters
// AC3: eval-current exercises incremental dirty path
// AC4: multi-round mutate matrix monotonic
// AC5: query:dirty-subtree regression reachable
// AC6: query regression (dirty-reason-propagation-stats,
//      typed-mutation-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_408_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t dirty_cost_stats(CompilerService& cs) {
    auto r = cs.eval("(query:dirty-propagation-cost-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define base 10) (define acc 0) "
                 "(add1 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:dirty-propagation-cost-stats ---");
    CHECK(setup_workspace(cs), "dirty propagation workspace setup");
    const auto s0 = dirty_cost_stats(cs);
    std::println("  dirty-propagation-cost-stats = {}", s0);
    CHECK(s0 >= 0, "dirty cost stats non-negative");

    std::println("\n--- AC2: mutate:rebind bumps propagation ---");
    const auto stats2a = dirty_cost_stats(cs);
    (void)cs.eval("(mutate:rebind \"base\" \"99\")");
    const auto stats2b = dirty_cost_stats(cs);
    std::println("  dirty cost stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b > stats2a, "mutate bumps dirty propagation cost stats");

    std::println("\n--- AC3: eval-current incremental dirty path ---");
    const auto stats3a = dirty_cost_stats(cs);
    CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
    const auto stats3b = dirty_cost_stats(cs);
    std::println("  dirty cost stats: {} -> {}", stats3a, stats3b);
    CHECK(stats3b >= stats3a, "eval monotonic for dirty cost stats");

    std::println("\n--- AC4: multi-round mutate matrix ---");
    const auto stats4a = dirty_cost_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats4b = dirty_cost_stats(cs);
    std::println("  dirty cost stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b >= stats4a, "dirty cost stats monotonic over matrix");

    std::println("\n--- AC5: query:dirty-subtree regression ---");
    auto dst = cs.eval("(query:dirty-subtree)");
    CHECK(dst.has_value(), "query:dirty-subtree returns value");

    std::println("\n--- AC6: query regression ---");
    auto drp = cs.eval("(query:dirty-reason-propagation-stats)");
    auto tms = cs.eval("(engine:metrics \"query:typed-mutation-stats\")");
    CHECK(drp && is_int(*drp), "dirty-reason-propagation-stats regression");
    CHECK(tms && is_int(*tms), "typed-mutation-stats regression");
}

} // namespace aura_408_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_408_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}