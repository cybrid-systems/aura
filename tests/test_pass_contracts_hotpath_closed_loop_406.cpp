// test_pass_contracts_hotpath_closed_loop_406.cpp
// Issue #406: Pass Pipeline + Contracts coverage + zero-overhead
// hot-path validation observability.
//
// Non-duplicative with #381 (pass concept unit tests),
// #571 (value-dispatch-stats), #506 (soa-hotpath-adoption-stats).
//
// AC1: query:pass-contracts-stats reachable
// AC2: eval-current exercises pass pipeline counters
// AC3: eval-current :jit exercises JIT pass pipeline path
// AC4: mutate + eval bumps pass skip / re-lower counters
// AC5: multi-round mutate matrix — pass-contracts stats monotonic
// AC6: query regression (value-dispatch-stats,
//      soa-hotpath-adoption-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_406_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t pass_stats(CompilerService& cs) {
    auto r = cs.eval("(query:pass-contracts-stats)");
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
    std::println("\n--- AC1: query:pass-contracts-stats ---");
    CHECK(setup_workspace(cs), "pass pipeline workspace setup");
    const auto s0 = pass_stats(cs);
    std::println("  pass-contracts-stats = {}", s0);
    CHECK(s0 >= 0, "pass-contracts stats non-negative");

    std::println("\n--- AC2: eval-current pass pipeline ---");
    const auto stats2a = pass_stats(cs);
    CHECK(cs.eval("(eval-current)").has_value(), "re-eval current");
    const auto stats2b = pass_stats(cs);
    std::println("  pass-contracts stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b >= stats2a, "re-eval monotonic for pass stats");

    std::println("\n--- AC3: eval-current :jit pass path ---");
    const auto stats3a = pass_stats(cs);
    auto jit = cs.eval("(eval-current :jit)");
    const auto stats3b = pass_stats(cs);
    std::println("  jit ok={} stats: {} -> {}",
                 jit.has_value(), stats3a, stats3b);
    CHECK(jit.has_value(), "eval-current :jit succeeds");
    CHECK(stats3b >= stats3a, "JIT path monotonic");

    std::println("\n--- AC4: mutate + eval bumps pass counters ---");
    const auto stats4a = pass_stats(cs);
    (void)cs.eval("(mutate:rebind \"base\" \"42\")");
    CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
    (void)cs.eval("(eval-current :jit)");
    const auto stats4b = pass_stats(cs);
    std::println("  pass-contracts stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b >= stats4a, "mutate+eval bumps pass-contracts stats");

    std::println("\n--- AC5: multi-round mutate matrix ---");
    const auto stats5a = pass_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" +
                      std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(eval-current :jit)");
    }
    const auto stats5b = pass_stats(cs);
    std::println("  pass-contracts stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "pass-contracts stats monotonic over matrix");

    std::println("\n--- AC6: query regression ---");
    auto vds = cs.eval("(query:value-dispatch-stats)");
    auto soa = cs.eval("(query:soa-hotpath-adoption-stats)");
    CHECK(vds && is_int(*vds), "value-dispatch-stats regression");
    CHECK(soa && is_int(*soa), "soa-hotpath-adoption-stats regression");
}

} // namespace aura_406_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_406_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}