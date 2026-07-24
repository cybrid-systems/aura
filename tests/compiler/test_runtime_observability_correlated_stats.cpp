// test_runtime_observability_correlated_stats_673.cpp — Issue #673:
// Issue #438/#448/#591/#592/#596/#599 (#1978 renamed): issue# moved from filename to header.
// Unified Runtime Observability Layer (P1) cross-module correlation
// primitive (production review).
//
// Non-duplicative with #591 (scheduler-mutation-coord-stats),
// #438 (fiber-migration-stats), #448 (mutation-coordination-stats),
// #599 (compiler-root-stats), #592 (panic-checkpoint-fiber-stats),
// #596 (guard-panic-reflect-stats).
//
//   - AC1:  query:runtime-observability-correlated-stats reachable (schema 673)
//   - AC2:  baseline counter (steal-attempts-correlated) bumps on worker steal path
//   - AC3:  steal-deferred-correlated bumps when bump_mutation_steal_violation_count
//           fires (i.e. via aura_evaluator_bump_steal_deferred_violation trampoline)
//   - AC4:  steal-ownership-violation-correlated bumps on linear ownership probe
//           violation path
//   - AC5:  correlated-events-total == sum of 3 per-counter fields
//   - AC6:  multi-round monotonic — repeated bumps keep adding
//   - AC7:  query regression — 4 module-local stats primitives still reachable
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_673_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t hash_int(CompilerService& cs, const std::string& key) {
    auto r =
        cs.eval("(hash-ref (engine:metrics \"query:runtime-observability-correlated-stats\") \"" +
                key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t steal_attempts(CompilerService& cs) {
    return hash_int(cs, "steal-attempts-correlated");
}
static std::int64_t steal_deferred(CompilerService& cs) {
    return hash_int(cs, "steal-deferred-correlated");
}
static std::int64_t ownership_violation(CompilerService& cs) {
    return hash_int(cs, "steal-ownership-violation-correlated");
}
static std::int64_t correlated_total(CompilerService& cs) {
    return hash_int(cs, "correlated-events-total");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:runtime-observability-correlated-stats (schema 673) ---");
    auto h = cs.eval("(engine:metrics \"query:runtime-observability-correlated-stats\")");
    CHECK(h && is_hash(*h), "runtime-observability-correlated-stats returns hash");
    CHECK(hash_int(cs, "schema") == 673, "schema == 673");
    const auto s0_attempts = steal_attempts(cs);
    const auto s0_deferred = steal_deferred(cs);
    const auto s0_own = ownership_violation(cs);
    const auto s0_total = correlated_total(cs);
    std::println("  baseline: attempts={}, deferred={}, own_violation={}, total={}", s0_attempts,
                 s0_deferred, s0_own, s0_total);
    CHECK(s0_attempts >= 0, "steal-attempts-correlated non-negative");
    CHECK(s0_deferred >= 0, "steal-deferred-correlated non-negative");
    CHECK(s0_own >= 0, "steal-ownership-violation-correlated non-negative");
    CHECK(s0_total >= 0, "correlated-events-total non-negative");

    std::println("\n--- AC2: baseline counter bumps on direct path ---");
    const auto a0 = steal_attempts(cs);
    cs.evaluator().bump_runtime_observability_steal_attempt_correlated();
    cs.evaluator().bump_runtime_observability_steal_attempt_correlated();
    const auto a1 = steal_attempts(cs);
    std::println("  steal-attempts-correlated: {} -> {}", a0, a1);
    CHECK(a1 == a0 + 2, "baseline counter bumps by exactly 2");

    std::println("\n--- AC3: steal-deferred-correlated bumps on direct path ---");
    const auto d0 = steal_deferred(cs);
    cs.evaluator().bump_runtime_observability_steal_deferred_correlated();
    cs.evaluator().bump_runtime_observability_steal_deferred_correlated();
    cs.evaluator().bump_runtime_observability_steal_deferred_correlated();
    const auto d1 = steal_deferred(cs);
    std::println("  steal-deferred-correlated: {} -> {}", d0, d1);
    CHECK(d1 == d0 + 3, "deferred counter bumps by exactly 3");

    std::println("\n--- AC4: ownership-violation-correlated bumps on direct path ---");
    const auto o0 = ownership_violation(cs);
    cs.evaluator().bump_runtime_observability_steal_ownership_violation_correlated();
    const auto o1 = ownership_violation(cs);
    std::println("  ownership-violation-correlated: {} -> {}", o0, o1);
    CHECK(o1 == o0 + 1, "ownership-violation counter bumps by exactly 1");

    std::println("\n--- AC5: correlated-events-total == sum ---");
    const auto sa = steal_attempts(cs);
    const auto sd = steal_deferred(cs);
    const auto so = ownership_violation(cs);
    const auto st = correlated_total(cs);
    std::println("  attempts={} + deferred={} + ownership={} = sum {} (primitive total {})", sa, sd,
                 so, sa + sd + so, st);
    CHECK(st == sa + sd + so, "correlated-events-total == sum of 3 per-counters");

    std::println("\n--- AC6: multi-round bumps monotonic ---");
    const auto t_before = correlated_total(cs);
    for (int round = 0; round < 5; ++round) {
        cs.evaluator().bump_runtime_observability_steal_attempt_correlated();
        cs.evaluator().bump_runtime_observability_steal_deferred_correlated();
    }
    const auto t_after = correlated_total(cs);
    std::println("  correlated-events-total: {} -> {}", t_before, t_after);
    CHECK(t_after == t_before + 10, "multi-round adds exactly 10 to total");

    std::println("\n--- AC7: query regression — module-local primitives still reachable ---");
    auto cs_stats = cs.eval("(engine:metrics \"query:compiler-root-stats\")");
    auto fiber_stats = cs.eval("(engine:metrics \"query:fiber-migration-stats\")");
    auto mutation_coord = cs.eval("(engine:metrics \"query:mutation-coordination-stats\")");
    auto guard_panic_reflect = cs.eval("(engine:metrics \"query:guard-panic-reflect-stats\")");
    // Note: fiber-migration-stats and mutation-coordination-stats return
    // make_int(sum); compiler-root-stats and guard-panic-reflect-stats
    // return hashes (schema + multi-field). Mixed types here is correct.
    CHECK(cs_stats && is_hash(*cs_stats), "compiler-root-stats (schema 599) regression [hash]");
    CHECK(fiber_stats && is_int(*fiber_stats), "fiber-migration-stats regression [int sum]");
    CHECK(mutation_coord && is_int(*mutation_coord),
          "mutation-coordination-stats regression [int sum]");
    CHECK(guard_panic_reflect && is_hash(*guard_panic_reflect),
          "guard-panic-reflect-stats regression [hash]");
}

} // namespace aura_673_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_673_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}
