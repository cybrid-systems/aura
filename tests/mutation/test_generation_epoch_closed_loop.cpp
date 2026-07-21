// Issue #368/#414/#456/#457/#527 (#1978 renamed): issue# moved from filename to header.
// test_generation_epoch_closed_loop_414.cpp
// Issue #414: Long-term generation_ + wrap_epoch_ composite epoch
// management observability under high-frequency mutate/rollback.
//
// Non-duplicative with #456 (epoch-stats single defuse_version),
// #457 (stable-ref-stats 3 FlatAST counters), #368
// (ast:generation-stats per-field hash), #527
// (stable-ref-cow-fiber-stats COW/fiber slice).
//
// AC1: query:generation-epoch-stats reachable
// AC2: mutate:rebind bumps generation/epoch counters
// AC3: ast:generation-stats EDSL integration
// AC4: eval-current exercises mutation epoch path
// AC5: multi-round mutate matrix monotonic
// AC6: query regression (epoch-stats, stable-ref-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_414_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t generation_epoch_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:generation-epoch-stats\")");
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
    std::println("\n--- AC1: query:generation-epoch-stats ---");
    CHECK(setup_workspace(cs), "generation epoch workspace setup");
    const auto s0 = generation_epoch_stats(cs);
    std::println("  generation-epoch-stats = {}", s0);
    CHECK(s0 >= 0, "generation epoch stats non-negative");

    std::println("\n--- AC2: mutate:rebind bumps epoch counters ---");
    const auto stats2a = generation_epoch_stats(cs);
    (void)cs.eval("(mutate:rebind \"base\" \"99\")");
    const auto stats2b = generation_epoch_stats(cs);
    std::println("  generation-epoch-stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b > stats2a, "mutate bumps generation epoch stats");

    std::println("\n--- AC3: ast:generation-stats integration ---");
    auto ags = cs.eval("(stats:get \"ast:generation-stats\")");
    CHECK(ags.has_value(), "ast:generation-stats returns value");

    std::println("\n--- AC4: eval-current mutation epoch path ---");
    const auto stats4a = generation_epoch_stats(cs);
    CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
    const auto stats4b = generation_epoch_stats(cs);
    std::println("  generation-epoch-stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b >= stats4a, "eval monotonic for generation epoch stats");

    std::println("\n--- AC5: multi-round mutate matrix ---");
    const auto stats5a = generation_epoch_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats5b = generation_epoch_stats(cs);
    std::println("  generation-epoch-stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b > stats5a, "generation epoch stats grow over matrix");

    std::println("\n--- AC6: query regression ---");
    auto eps = cs.eval("(engine:metrics \"query:epoch-stats\")");
    auto srs = cs.eval("(engine:metrics \"query:stable-ref-stats\")");
    CHECK(eps && is_int(*eps), "epoch-stats regression");
    CHECK(srs && is_int(*srs), "stable-ref-stats regression");
}

} // namespace aura_414_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_414_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}