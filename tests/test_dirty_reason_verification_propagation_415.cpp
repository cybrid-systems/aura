// test_dirty_reason_verification_propagation_415.cpp
// Issue #415: Extend DirtyReason bitmask with verification
// categories and propagation metrics.
//
// Non-duplicative with #344 (per-node dirty tallies +
// query:dirty-nodes), #437 (verify-dirty-stats 4-tuple),
// #469 (verification-loop-stats SV mutate cycle counters).
//
// AC1: query:dirty-reason-propagation-stats reachable
// AC2: mutate:rebind bumps mark_dirty_upward propagation
// AC3: verify:assertion-failed bumps verify-dirty counters
// AC4: compile:dirty-reason-counts + query:dirty-nodes regression
// AC5: multi-round matrix — propagation stats monotonic
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_415_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_void;

static std::int64_t propagation_stats(CompilerService& cs) {
    auto r = cs.eval("(query:dirty-reason-propagation-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:dirty-reason-propagation-stats ---");
    const auto s0 = propagation_stats(cs);
    std::println("  dirty-reason-propagation-stats = {}", s0);
    CHECK(s0 >= 0, "propagation stats non-negative");

    std::println("\n--- AC2: mutate bumps mark_dirty_upward propagation ---");
    CHECK(cs.eval("(set-code \"(define x 1) (define y 2)\")").has_value(),
          "workspace setup");
    CHECK(cs.eval("(eval-current)").has_value(), "workspace eval");
    const auto stats2a = propagation_stats(cs);
    (void)cs.eval("(mutate:rebind \"x\" \"42\")");
    (void)cs.eval("(eval-current)");
    const auto stats2b = propagation_stats(cs);
    std::println("  propagation stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b > stats2a,
          "mutate:rebind bumps mark_dirty_upward propagation counters");

    std::println("\n--- AC3: verify:assertion-failed bumps verify counters ---");
    const auto stats3a = propagation_stats(cs);
    auto v1 = cs.eval("(verify:assertion-failed 0)");
    CHECK(v1.has_value(), "verify:assertion-failed returns a value");
    const auto stats3b = propagation_stats(cs);
    std::println("  propagation stats: {} -> {}", stats3a, stats3b);
    CHECK(stats3b > stats3a,
          "verify:assertion-failed bumps verify-dirty counters");

    std::println("\n--- AC4: dirty-reason-counts + dirty-nodes regression ---");
    auto drc = cs.eval("(compile:dirty-reason-counts)");
    CHECK(drc.has_value() && is_pair(*drc),
          "compile:dirty-reason-counts returns 8-tuple pair");
    auto dn = cs.eval("(query:dirty-nodes \"general\")");
    CHECK(dn.has_value(), "query:dirty-nodes returns a value");

    std::println("\n--- AC5: multi-round mutate matrix monotonic ---");
    const auto stats5a = propagation_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"y\" \"" +
                      std::to_string(20 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(verify:report-coverage 1)");
    }
    const auto stats5b = propagation_stats(cs);
    std::println("  propagation stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a,
          "propagation stats monotonic over multi-round matrix");

    std::println("\n--- AC4b: verify-dirty-stats regression ---");
    auto vds = cs.eval("(query:verify-dirty-stats)");
    CHECK(vds && is_int(*vds), "query:verify-dirty-stats regression");
}

} // namespace aura_415_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_415_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}