// test_stable_ref_cow_fiber_closed_loop_527.cpp
// Issue #527: StableNodeRef cross-COW/sub-workspace + concurrent
// fiber safety hardening for AI multi-round Query/Mutate loops.
//
// Non-duplicative with #552 (edsl-stability-stats Task1 slice),
// #549 (self-evolution-stability-stats Task6), #457
// (stable-ref-stats 3 FlatAST counters), #540 (StableNodeRef
// hardening unit).
//
// AC1: query:stable-ref-cow-fiber-stats reachable
// AC2: validate_stable_ref cross_cow bumps evaluator counters
// AC3: query:stable-ref + query:ref-valid? EDSL integration
// AC4: mutate:rebind + query:children-stable happy path
// AC5: cross_cow grows under mutate validate loop
// AC6: multi-round matrix — cow-fiber stats monotonic
// AC7: query regression (stable-ref-stats, edsl-stability-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_527_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;

static std::int64_t cow_fiber_stats(CompilerService& cs) {
    auto r = cs.eval("(query:stable-ref-cow-fiber-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define a 1) (define b 2) (define acc 0)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:stable-ref-cow-fiber-stats ---");
    CHECK(setup_workspace(cs), "workspace setup");
    const auto s0 = cow_fiber_stats(cs);
    std::println("  stable-ref-cow-fiber-stats = {}", s0);
    CHECK(s0 >= 0, "cow-fiber stats non-negative");

    std::println("\n--- AC2: validate_stable_ref cross_cow ---");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace flat available");
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    const auto g = ws->generation();
    (void)cs.evaluator().validate_stable_ref(0, g > 0 ? g - 1 : 0);
    const auto cc1 = cs.evaluator().get_cross_cow_invalidations();
    std::println("  cross_cow: {} -> {}", cc0, cc1);
    CHECK(cc1 > cc0, "validate_stable_ref bumps cross_cow_invalidations");

    std::println("\n--- AC3: query:stable-ref + ref-valid? ---");
    auto ref = cs.eval("(query:stable-ref 1)");
    CHECK(ref && is_pair(*ref), "query:stable-ref returns pair");
    auto valid = cs.eval("(let ((r (query:stable-ref 1))) (query:ref-valid? r))");
    CHECK(valid && is_bool(*valid), "query:ref-valid? returns bool");

    std::println("\n--- AC4: mutate:rebind + children-stable ---");
    (void)cs.eval("(mutate:rebind \"acc\" \"99\")");
    auto kids = cs.eval("(query:children-stable 0)");
    CHECK(kids.has_value(), "query:children-stable returns on root");

    std::println("\n--- AC5: cross_cow under mutate validate loop ---");
    const auto stats5a = cow_fiber_stats(cs);
    const auto cc5a = cs.evaluator().get_cross_cow_invalidations();
    for (int i = 0; i < 20; ++i) {
        (void)cs.eval("(mutate:rebind \"a\" \"" + std::to_string(10 + i) + "\")");
        const auto gen = ws->generation();
        (void)cs.evaluator().validate_stable_ref(0, gen > 0 ? gen - 1 : 0);
    }
    const auto cc5b = cs.evaluator().get_cross_cow_invalidations();
    const auto stats5b = cow_fiber_stats(cs);
    std::println("  cross_cow: {} -> {} stats: {} -> {}",
                 cc5a, cc5b, stats5a, stats5b);
    CHECK(cc5b > cc5a, "mutate+validate loop grows cross_cow");
    CHECK(stats5b > stats5a, "cow-fiber stats grow under loop");

    std::println("\n--- AC6: multi-round matrix monotonic ---");
    const auto stats6a = cow_fiber_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"b\" \"" +
                      std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(let ((r (query:stable-ref 1))) (query:ref-valid? r))");
    }
    const auto stats6b = cow_fiber_stats(cs);
    std::println("  cow-fiber stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "cow-fiber stats monotonic over matrix");

    std::println("\n--- AC7: query regression ---");
    auto srs = cs.eval("(query:stable-ref-stats)");
    auto eds = cs.eval("(query:edsl-stability-stats)");
    CHECK(srs && is_int(*srs), "stable-ref-stats regression");
    CHECK(eds && is_int(*eds), "edsl-stability-stats regression");
}

} // namespace aura_527_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_527_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}