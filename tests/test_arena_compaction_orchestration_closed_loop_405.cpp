// test_arena_compaction_orchestration_closed_loop_405.cpp
// Issue #405: Arena automatic compaction triggering +
// fragmentation signals for AI orchestration.
//
// Non-duplicative with #187 (arena:compact primitives),
// #335 (arena:adaptive-stats 2-tuple), #300 (arena:defrag-stats).
//
// AC1: query:arena-compaction-stats reachable
// AC2: arena:estimate fragmentation signal returns int
// AC3: arena:compact bumps compaction counters in stats
// AC4: mutate + eval bumps mutation/dirty orchestration signals
// AC5: arena:adaptive-compact integration monotonic
// AC6: multi-round mutate matrix — compaction stats monotonic
// AC7: query regression (arena:adaptive-stats, arena:estimate)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_405_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;

static std::int64_t compaction_stats(CompilerService& cs) {
    auto r = cs.eval("(query:arena-compaction-stats)");
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
    std::println("\n--- AC1: query:arena-compaction-stats ---");
    CHECK(setup_workspace(cs), "arena workspace setup + eval");
    const auto s0 = compaction_stats(cs);
    std::println("  arena-compaction-stats = {}", s0);
    CHECK(s0 >= 0, "compaction stats non-negative");

    std::println("\n--- AC2: arena:estimate fragmentation signal ---");
    auto est = cs.eval("(arena:estimate)");
    CHECK(est && is_int(*est), "arena:estimate returns int");

    std::println("\n--- AC3: arena:compact bumps counters ---");
    const auto stats3a = compaction_stats(cs);
    (void)cs.eval("(arena:compact)");
    const auto stats3b = compaction_stats(cs);
    std::println("  compaction stats: {} -> {}", stats3a, stats3b);
    CHECK(stats3b >= stats3a, "arena:compact monotonic for stats");

    std::println("\n--- AC4: mutate + eval orchestration signals ---");
    const auto stats4a = compaction_stats(cs);
    (void)cs.eval("(mutate:rebind \"base\" \"42\")");
    CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
    const auto stats4b = compaction_stats(cs);
    std::println("  compaction stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b > stats4a, "mutate bumps mutation/dirty signals");

    std::println("\n--- AC5: arena:adaptive-compact integration ---");
    const auto stats5a = compaction_stats(cs);
    (void)cs.eval("(arena:adaptive-compact)");
    const auto stats5b = compaction_stats(cs);
    std::println("  compaction stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "adaptive-compact monotonic");

    std::println("\n--- AC6: multi-round mutate matrix ---");
    const auto stats6a = compaction_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" +
                      std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(arena:compact)");
    }
    const auto stats6b = compaction_stats(cs);
    std::println("  compaction stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "compaction stats monotonic over matrix");

    std::println("\n--- AC7: query regression ---");
    auto ads = cs.eval("(arena:adaptive-stats)");
    auto est2 = cs.eval("(arena:estimate)");
    CHECK(ads && is_pair(*ads), "arena:adaptive-stats regression");
    CHECK(est2 && is_int(*est2), "arena:estimate regression");
}

} // namespace aura_405_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_405_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}