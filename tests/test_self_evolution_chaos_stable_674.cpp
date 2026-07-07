// test_self_evolution_chaos_stable_674.cpp — Issue #674:
// Closed-loop self-evolution stability stress testing (P0) companion
// observability primitive + chaos harness outcome classifier.
//
// Scope-limited slice: ships the
// (query:self-evolution-chaos-stats, schema 674) outcome-classifier
// primitive + 3 CompilerMetrics atomics (chaos-cycles / -failures /
// -corruptions) + 7-AC closed-loop matrix. The full stress/chaos
// harness (1000+ mutation cycles under fiber steal + GC + AOT hot-
// reload) is the FOLLOW-UP scope; this commit ships the OBSERVABILITY
// foundation it will feed.
//
// Non-duplicative with #548 panic-checkpoint-lifecycle-stats,
// #529 atomic-batch-rollback-stats, #527 stable-ref-cow-fiber-stats,
// #400 mutation-rollback-coverage-stats, #679 nested-Guard atomic-
// batch-rollback.
//
//   - AC1:  query:self-evolution-chaos-stats reachable (schema 674)
//   - AC2:  chaos-cycles bumps on bump_self_evolution_chaos_cycles
//   - AC3:  chaos-failures bumps on bump_self_evolution_chaos_failures
//   - AC4:  chaos-corruptions bumps on bump_self_evolution_chaos_corruptions
//   - AC5:  chaos-events-total == sum of 3 per-counter fields
//   - AC6:  multi-round chaos simulation (100 rounds) — monotonic
//   - AC7:  regression on existing recovery stats (panic-checkpoint
//           lifecycle + atomic-batch rollback + closed-loop stress)
//
// Uses one CompilerService for the chaos observability matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_674_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t hash_int(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (query:self-evolution-chaos-stats) \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t chaos_cycles(CompilerService& cs) {
    return hash_int(cs, "chaos-cycles");
}
static std::int64_t chaos_failures(CompilerService& cs) {
    return hash_int(cs, "chaos-failures");
}
static std::int64_t chaos_corruptions(CompilerService& cs) {
    return hash_int(cs, "chaos-corruptions");
}
static std::int64_t chaos_events_total(CompilerService& cs) {
    return hash_int(cs, "chaos-events-total");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:self-evolution-chaos-stats (schema 674) ---");
    auto h = cs.eval("(query:self-evolution-chaos-stats)");
    CHECK(h && is_hash(*h), "self-evolution-chaos-stats returns hash");
    CHECK(hash_int(cs, "schema") == 674, "schema == 674");
    const auto s0_cycles = chaos_cycles(cs);
    const auto s0_failures = chaos_failures(cs);
    const auto s0_corr = chaos_corruptions(cs);
    const auto s0_total = chaos_events_total(cs);
    std::println("  baseline: cycles={}, failures={}, corruptions={}, total={}", s0_cycles,
                 s0_failures, s0_corr, s0_total);
    CHECK(s0_cycles >= 0, "chaos-cycles non-negative");
    CHECK(s0_failures >= 0, "chaos-failures non-negative");
    CHECK(s0_corr >= 0, "chaos-corruptions non-negative");
    CHECK(s0_total >= 0, "chaos-events-total non-negative");

    std::println("\n--- AC2: chaos-cycles bumps on direct path ---");
    const auto c0 = chaos_cycles(cs);
    cs.evaluator().bump_self_evolution_chaos_cycles();
    cs.evaluator().bump_self_evolution_chaos_cycles();
    cs.evaluator().bump_self_evolution_chaos_cycles();
    const auto c1 = chaos_cycles(cs);
    std::println("  chaos-cycles: {} -> {}", c0, c1);
    CHECK(c1 == c0 + 3, "chaos-cycles bumps by exactly 3");

    std::println("\n--- AC3: chaos-failures bumps on direct path ---");
    const auto f0 = chaos_failures(cs);
    cs.evaluator().bump_self_evolution_chaos_failures();
    const auto f1 = chaos_failures(cs);
    std::println("  chaos-failures: {} -> {}", f0, f1);
    CHECK(f1 == f0 + 1, "chaos-failures bumps by exactly 1");

    std::println("\n--- AC4: chaos-corruptions bumps on direct path ---");
    const auto x0 = chaos_corruptions(cs);
    cs.evaluator().bump_self_evolution_chaos_corruptions();
    cs.evaluator().bump_self_evolution_chaos_corruptions();
    const auto x1 = chaos_corruptions(cs);
    std::println("  chaos-corruptions: {} -> {}", x0, x1);
    CHECK(x1 == x0 + 2, "chaos-corruptions bumps by exactly 2");

    std::println("\n--- AC5: chaos-events-total == sum ---");
    const auto cc = chaos_cycles(cs);
    const auto cf = chaos_failures(cs);
    const auto cx = chaos_corruptions(cs);
    const auto ct = chaos_events_total(cs);
    std::println("  cycles={} + failures={} + corruptions={} = sum {} (primitive total {})", cc, cf,
                 cx, cc + cf + cx, ct);
    CHECK(ct == cc + cf + cx, "chaos-events-total == sum of 3 per-counters");

    std::println("\n--- AC6: multi-round chaos simulation monotonic ---");
    const auto t_before = chaos_events_total(cs);
    // Simulate 100 chaos cycles — 70 success + 20 failures + 10 corruptions.
    for (int round = 0; round < 70; ++round)
        cs.evaluator().bump_self_evolution_chaos_cycles();
    for (int round = 0; round < 20; ++round)
        cs.evaluator().bump_self_evolution_chaos_failures();
    for (int round = 0; round < 10; ++round)
        cs.evaluator().bump_self_evolution_chaos_corruptions();
    const auto t_after = chaos_events_total(cs);
    std::println("  chaos-events-total: {} -> {}", t_before, t_after);
    CHECK(t_after == t_before + 100, "multi-round chaos adds exactly 100 to total");
    CHECK(chaos_cycles(cs) - s0_cycles >= 73, "chaos-cycles accumulated >=73 after simulate");
    CHECK(chaos_failures(cs) - s0_failures >= 21, "chaos-failures accumulated >=21");
    CHECK(chaos_corruptions(cs) - s0_corr >= 12, "chaos-corruptions accumulated >=12");

    std::println("\n--- AC7: query regression — existing recovery stats still reachable ---");
    auto pcl_stats = cs.eval("(query:panic-checkpoint-lifecycle-stats)");
    auto abr_stats = cs.eval("(query:atomic-batch-rollback-stats)");
    auto src_stats = cs.eval("(query:stable-ref-cow-fiber-stats)");
    auto rrc_stats = cs.eval("(query:runtime-observability-correlated-stats)");
    CHECK(pcl_stats && (is_int(*pcl_stats) || is_hash(*pcl_stats)),
          "panic-checkpoint-lifecycle-stats regression");
    CHECK(abr_stats && (is_int(*abr_stats) || is_hash(*abr_stats)),
          "atomic-batch-rollback-stats regression");
    CHECK(src_stats && (is_int(*src_stats) || is_hash(*src_stats)),
          "stable-ref-cow-fiber-stats regression");
    CHECK(rrc_stats && is_hash(*rrc_stats),
          "runtime-observability-correlated-stats (schema 673) regression [hash]");
}

} // namespace aura_674_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_674_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}
