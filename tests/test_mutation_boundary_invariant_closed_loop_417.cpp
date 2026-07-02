// test_mutation_boundary_invariant_closed_loop_417.cpp
// Issue #417: Post P1/P2 cross-TU MutationBoundaryGuard +
// defuse_version_ + per-fiber stack invariant observability.
//
// Non-duplicative with #448 (mutation-coordination-stats),
// #438 (fiber-migration-stats), #264 (compile:concurrency-stats).
//
// AC1: query:mutation-boundary-invariant-stats reachable
// AC2: mutate:rebind bumps boundary epoch counters
// AC3: nested enter/exit_mutation_boundary integration
// AC4: ensure_mutation_invariants — zero violations on happy path
// AC5: eval-current exercises materialize_call_env probe
// AC6: multi-round mutate matrix monotonic
// AC7: query regression (mutation-coordination-stats,
//      fiber-migration-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_417_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t boundary_invariant_stats(CompilerService& cs) {
    auto r = cs.eval("(query:mutation-boundary-invariant-stats)");
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
    std::println("\n--- AC1: query:mutation-boundary-invariant-stats ---");
    CHECK(setup_workspace(cs), "mutation boundary workspace setup");
    const auto s0 = boundary_invariant_stats(cs);
    std::println("  mutation-boundary-invariant-stats = {}", s0);
    CHECK(s0 >= 0, "boundary invariant stats non-negative");

    std::println("\n--- AC2: mutate:rebind bumps epoch counters ---");
    const auto stats2a = boundary_invariant_stats(cs);
    (void)cs.eval("(mutate:rebind \"base\" \"99\")");
    const auto stats2b = boundary_invariant_stats(cs);
    std::println("  boundary invariant stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b > stats2a, "mutate bumps boundary invariant stats");

    std::println("\n--- AC3: nested enter/exit_mutation_boundary ---");
    auto& ev = cs.evaluator();
    ev.enter_mutation_boundary();
    ev.enter_mutation_boundary();
    ev.exit_mutation_boundary(true);
    ev.exit_mutation_boundary(true);
    CHECK(ev.get_total_invariant_violations() == 0,
          "nested boundary exit: zero invariant violations");

    std::println("\n--- AC4: ensure_mutation_invariants happy path ---");
    ev.ensure_mutation_invariants();
    CHECK(ev.get_total_invariant_violations() == 0,
          "explicit probe: zero invariant violations");

    std::println("\n--- AC5: eval-current materialize_call_env probe ---");
    const auto stats5a = boundary_invariant_stats(cs);
    CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
    const auto stats5b = boundary_invariant_stats(cs);
    std::println("  boundary invariant stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "eval monotonic for boundary invariant stats");

    std::println("\n--- AC6: multi-round mutate matrix ---");
    const auto stats6a = boundary_invariant_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" +
                      std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats6b = boundary_invariant_stats(cs);
    std::println("  boundary invariant stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b > stats6a, "boundary invariant stats grow over matrix");
    CHECK(ev.get_total_invariant_violations() == 0,
          "matrix end: zero invariant violations");

    std::println("\n--- AC7: query regression ---");
    auto mcs = cs.eval("(query:mutation-coordination-stats)");
    auto fms = cs.eval("(query:fiber-migration-stats)");
    CHECK(mcs && is_int(*mcs), "mutation-coordination-stats regression");
    CHECK(fms && is_int(*fms), "fiber-migration-stats regression");
}

} // namespace aura_417_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_417_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}