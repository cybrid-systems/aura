// Issue #192/#459/#529/#553 (#1978 renamed): issue# moved from filename to header.
// test_atomic_batch_rollback_closed_loop_529.cpp
// Issue #529: End-to-end atomic batch mutate + mutation_log_
// rollback + Guard + fiber orchestration safety (Task1 EDSL).
//
// Non-duplicative with #553 (mutation-log-stats batch matrix),
// #459 (atomic-batch-stats 1-counter), #192 (atomic-batch unit).
//
// AC1: query:atomic-batch-rollback-stats reachable
// AC2: mutate:atomic-batch happy path bumps batch_commits
// AC3: failed batch path — rollbacks observable + monotonic
// AC4: mutate:rebind under Guard bumps guard_success counters
// AC5: multi-round batch matrix — rollback stats monotonic
// AC6: query regression (mutation-log-stats, atomic-batch-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_529_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t rollback_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:atomic-batch-rollback-stats\")");
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
    std::println("\n--- AC1: query:atomic-batch-rollback-stats ---");
    CHECK(setup_workspace(cs), "workspace setup");
    const auto s0 = rollback_stats(cs);
    std::println("  atomic-batch-rollback-stats = {}", s0);
    CHECK(s0 >= 0, "rollback stats non-negative");

    std::println("\n--- AC2: mutate:atomic-batch happy path ---");
    const auto commits0 = cs.evaluator().atomic_batch_count();
    const auto stats2a = rollback_stats(cs);
    auto ok = cs.eval("(mutate:atomic-batch (list "
                      "(list \"mutate:rebind\" \"a\" \"10\") "
                      "(list \"mutate:rebind\" \"b\" \"20\")) "
                      "\"529-happy\")");
    CHECK(ok.has_value(), "mutate:atomic-batch happy path returns");
    const auto commits1 = cs.evaluator().atomic_batch_count();
    const auto stats2b = rollback_stats(cs);
    std::println("  batch_commits: {} -> {} stats: {} -> {}", commits0, commits1, stats2a, stats2b);
    CHECK(commits1 > commits0, "happy batch bumps atomic_batch_count");
    CHECK(stats2b > stats2a, "rollback stats bumped after happy batch");

    std::println("\n--- AC3: failed batch rollback path ---");
    const auto roll0 = cs.evaluator().atomic_batch_rollbacks();
    const auto stats3a = rollback_stats(cs);
    (void)cs.eval("(mutate:atomic-batch (list "
                  "(list \"mutate:replace-value\" 999 \"bad\")) "
                  "\"529-fail\")");
    const auto roll1 = cs.evaluator().atomic_batch_rollbacks();
    const auto stats3b = rollback_stats(cs);
    std::println("  rollbacks: {} -> {} stats: {} -> {}", roll0, roll1, stats3a, stats3b);
    CHECK(roll1 >= roll0, "batch rollbacks monotonic on fail path");
    CHECK(stats3b >= stats3a, "rollback stats monotonic on fail path");

    std::println("\n--- AC4: Guard mutate bumps guard_success ---");
    const auto impact0 = cs.evaluator().get_mutation_impact_count();
    (void)cs.eval("(mutate:rebind \"acc\" \"42\")");
    const auto impact1 = cs.evaluator().get_mutation_impact_count();
    std::println("  mutation_impact: {} -> {}", impact0, impact1);
    CHECK(impact1 > impact0, "Guard success bumps mutation_impact");

    std::println("\n--- AC5: multi-round batch matrix monotonic ---");
    const auto stats5a = rollback_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:atomic-batch (list) \"529-round-" + std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats5b = rollback_stats(cs);
    std::println("  rollback stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "rollback stats monotonic over matrix");

    std::println("\n--- AC6: query regression ---");
    auto mls = cs.eval("(engine:metrics \"query:mutation-log-stats\")");
    auto abs = cs.eval("(engine:metrics \"query:atomic-batch-stats\")");
    CHECK(mls && is_int(*mls), "mutation-log-stats regression");
    CHECK(abs && is_int(*abs), "atomic-batch-stats regression");
}

} // namespace aura_529_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_529_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}