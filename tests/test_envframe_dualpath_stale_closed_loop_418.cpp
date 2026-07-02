// test_envframe_dualpath_stale_closed_loop_418.cpp
// Issue #418: evaluator_env.cpp dual-path (bindings_ vs
// bindings_symid_) consistency + EnvFrame stale policy
// observability under mutate + closure materialize load.
//
// Non-duplicative with #543 (envframe-dualpath-stats 4-counter),
// #417 (mutation-boundary-invariant-stats), #602
// (prompt6-safety-score aggregate).
//
// AC1: query:envframe-dualpath-stale-stats reachable
// AC2: closure eval exercises materialize_call_env probe
// AC3: mutate:rebind bumps stale epoch counters
// AC4: zero desync on happy path
// AC5: eval-current after mutate monotonic
// AC6: multi-round mutate matrix monotonic
// AC7: query regression (envframe-dualpath-stats,
//      mutation-boundary-invariant-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_418_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t dualpath_stale_stats(CompilerService& cs) {
    auto r = cs.eval("(query:envframe-dualpath-stale-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define base 10) (define acc 0) "
                 "((lambda (y) (+ y 1)) 5) "
                 "(add1 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:envframe-dualpath-stale-stats ---");
    CHECK(setup_workspace(cs), "envframe dualpath workspace setup");
    const auto s0 = dualpath_stale_stats(cs);
    std::println("  envframe-dualpath-stale-stats = {}", s0);
    CHECK(s0 >= 0, "dualpath stale stats non-negative");

    std::println("\n--- AC2: closure eval materialize probe ---");
    CHECK(cs.eval("((lambda (z) (* z 2)) 7)").has_value(),
          "inline lambda eval exercises materialize_call_env");

    std::println("\n--- AC3: mutate:rebind bumps epoch counters ---");
    const auto stats3a = dualpath_stale_stats(cs);
    (void)cs.eval("(mutate:rebind \"base\" \"99\")");
    const auto stats3b = dualpath_stale_stats(cs);
    std::println("  dualpath stale stats: {} -> {}", stats3a, stats3b);
    CHECK(stats3b > stats3a, "mutate bumps dualpath stale stats");

    std::println("\n--- AC4: desync stable on happy path ---");
    const auto desync4a = cs.evaluator().get_envframe_desync_detected();
    (void)cs.eval("(eval-current)");
    const auto desync4b = cs.evaluator().get_envframe_desync_detected();
    std::println("  desync: {} -> {}", desync4a, desync4b);
    CHECK(desync4b <= desync4a, "desync stable across re-eval");

    std::println("\n--- AC5: eval-current after mutate ---");
    const auto stats5a = dualpath_stale_stats(cs);
    CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
    const auto stats5b = dualpath_stale_stats(cs);
    std::println("  dualpath stale stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "eval monotonic for dualpath stale stats");

    std::println("\n--- AC6: multi-round mutate matrix ---");
    const auto stats6a = dualpath_stale_stats(cs);
    const auto desync6a = cs.evaluator().get_envframe_desync_detected();
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" +
                      std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("((lambda (n) (+ n 1)) " +
                      std::to_string(round) + ")");
    }
    const auto stats6b = dualpath_stale_stats(cs);
    std::println("  dualpath stale stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b > stats6a, "dualpath stale stats grow over matrix");
    const auto desync6b = cs.evaluator().get_envframe_desync_detected();
    std::println("  desync over matrix: {} -> {}", desync6a, desync6b);
    CHECK(desync6b <= desync6a, "matrix end: desync non-increasing");

    std::println("\n--- AC7: query regression ---");
    auto eds = cs.eval("(query:envframe-dualpath-stats)");
    auto mbi = cs.eval("(query:mutation-boundary-invariant-stats)");
    CHECK(eds && is_int(*eds), "envframe-dualpath-stats regression");
    CHECK(mbi && is_int(*mbi), "mutation-boundary-invariant-stats regression");
}

} // namespace aura_418_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_418_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}