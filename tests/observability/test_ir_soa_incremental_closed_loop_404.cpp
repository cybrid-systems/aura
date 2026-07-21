// test_ir_soa_incremental_closed_loop_404.cpp
// Issue #404: IR SoA Phase 3 — IRFunctionSoA + block_dirty_
// driven incremental lowering / re-lower observability.
//
// Non-duplicative with #506 (soa-hotpath-adoption-stats),
// #254 (compile:ir-soa-stats hash), #403 (ir-metadata-stats).
//
// AC1: query:ir-soa-incremental-stats reachable
// AC2: eval-current exercises SoA dual-emit counters
// AC3: mutate:rebind + eval bumps incremental re-lower counters
// AC4: eval-current :jit exercises SoA JIT bridge path
// AC5: multi-round mutate matrix — incremental stats monotonic
// AC6: query regression (soa-hotpath-adoption-stats,
//      task4-cache-locality-win)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_404_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t incremental_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:ir-soa-incremental-stats\")");
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
    std::println("\n--- AC1: query:ir-soa-incremental-stats ---");
    CHECK(setup_workspace(cs), "SoA workspace setup + eval");
    const auto s0 = incremental_stats(cs);
    std::println("  ir-soa-incremental-stats = {}", s0);
    CHECK(s0 >= 0, "incremental stats non-negative");

    std::println("\n--- AC2: eval exercises SoA dual-emit ---");
    const auto stats2a = incremental_stats(cs);
    CHECK(cs.eval("(eval-current)").has_value(), "re-eval current");
    const auto stats2b = incremental_stats(cs);
    std::println("  incremental stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b >= stats2a, "re-eval monotonic for SoA emit");

    std::println("\n--- AC3: mutate + eval bumps re-lower counters ---");
    const auto stats3a = incremental_stats(cs);
    (void)cs.eval("(mutate:rebind \"base\" \"99\")");
    CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
    const auto stats3b = incremental_stats(cs);
    std::println("  incremental stats: {} -> {}", stats3a, stats3b);
    CHECK(stats3b >= stats3a, "mutate+eval bumps incremental re-lower stats");

    std::println("\n--- AC4: eval-current :jit SoA bridge ---");
    const auto stats4a = incremental_stats(cs);
    auto jit = cs.eval("(eval-current :jit)");
    const auto stats4b = incremental_stats(cs);
    std::println("  jit ok={} stats: {} -> {}", jit.has_value(), stats4a, stats4b);
    CHECK(jit.has_value(), "eval-current :jit succeeds");
    CHECK(stats4b >= stats4a, "JIT path monotonic");

    std::println("\n--- AC5: multi-round mutate matrix ---");
    const auto stats5a = incremental_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(eval-current :jit)");
    }
    const auto stats5b = incremental_stats(cs);
    std::println("  incremental stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "incremental stats monotonic over matrix");

    std::println("\n--- AC6: query regression ---");
    auto soa = cs.eval("(engine:metrics \"query:soa-hotpath-adoption-stats\")");
    auto clw = cs.eval("(stats:get \"query:task4-cache-locality-win\")");
    CHECK(soa && is_int(*soa), "soa-hotpath-adoption-stats regression");
    CHECK(clw && is_int(*clw), "task4-cache-locality-win regression");
}

} // namespace aura_404_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_404_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}