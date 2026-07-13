// @category: integration
// @reason: Issue #664 — SV DefUse incremental observability (P1).
//  Ships (query:sv-defuse-stats, schema 664) + 3 CompilerMetrics
//  atomics (sv_defuse_nested_modports_total,
//  sv_defuse_cross_refs_total, sv_defuse_incremental_updates_total)
//  + 3 bump helpers + 3 accessors. The production wiring for
//  Actions #1 (extend DefUse build for generate + cross-ref) and
//  #2 (post-mutate incremental rebuild hook) is the FOLLOW-UP
//  scope; this commit ships the observability surface + regression
//  net.
//
// Non-duplicative with #317 DefUse scope tracking, #337
// ShapeProfiler, #640/#663 verification feedback, #691 per-fn
// defuse index metrics, #661 interface structure, #663 hardware
// backend, #662 SVA mutate.
//
//   - AC1:  query:sv-defuse-stats reachable (schema 664)
//   - AC2:  nested-modports bumps on bump_sv_defuse_nested_modports
//   - AC3:  cross-refs bumps on bump_sv_defuse_cross_refs
//   - AC4:  incremental-updates bumps on bump_sv_defuse_incremental_updates
//   - AC5:  defuse-events-total == sum of 3 per-counter fields
//   - AC6:  multi-round simulate (20 nests + 15 cross-refs + 10
//           incremental) — monotonic, sums correct
//   - AC7:  regression — adjacent SV primitives (sv-interface 661,
//           sv-sva 694, hardware-backend-sv 663) all reachable
//           from same CompilerService (cross-feature sanity)

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_664_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:sv-defuse-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::int64_t nested(aura::compiler::CompilerService& cs) {
    return stat_int(cs, "nested-modports");
}
static std::int64_t cross(aura::compiler::CompilerService& cs) {
    return stat_int(cs, "cross-refs");
}
static std::int64_t incr(aura::compiler::CompilerService& cs) {
    return stat_int(cs, "incremental-updates");
}
static std::int64_t events_total(aura::compiler::CompilerService& cs) {
    return stat_int(cs, "defuse-events-total");
}

static void run_ac1_schema(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: query:sv-defuse-stats (schema 664) ---");
    auto h = cs.eval("(query:sv-defuse-stats)");
    CHECK(h && aura::compiler::types::is_hash(*h), "sv-defuse-stats returns hash");
    CHECK(stat_int(cs, "schema") == 664, "schema == 664");
    const auto n = nested(cs);
    const auto cr = cross(cs);
    const auto i = incr(cs);
    const auto t = events_total(cs);
    std::println("  baseline: nested={}, cross={}, incremental={}, total={}", n, cr, i, t);
    CHECK(n >= 0, "nested-modports non-negative");
    CHECK(cr >= 0, "cross-refs non-negative");
    CHECK(i >= 0, "incremental-updates non-negative");
    CHECK(t >= 0, "defuse-events-total non-negative");
}

static void run_ac2_nested(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: nested-modports bumps on direct path ---");
    const auto n0 = nested(cs);
    cs.evaluator().bump_sv_defuse_nested_modports();
    cs.evaluator().bump_sv_defuse_nested_modports();
    cs.evaluator().bump_sv_defuse_nested_modports();
    cs.evaluator().bump_sv_defuse_nested_modports();
    const auto n1 = nested(cs);
    std::println("  nested-modports: {} -> {}", n0, n1);
    CHECK(n1 == n0 + 4, "nested-modports bumps by exactly 4");
}

static void run_ac3_cross(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: cross-refs bumps on direct path ---");
    const auto c0 = cross(cs);
    cs.evaluator().bump_sv_defuse_cross_refs();
    const auto c1 = cross(cs);
    std::println("  cross-refs: {} -> {}", c0, c1);
    CHECK(c1 == c0 + 1, "cross-refs bumps by exactly 1");
}

static void run_ac4_incremental(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: incremental-updates bumps on direct path ---");
    const auto i0 = incr(cs);
    cs.evaluator().bump_sv_defuse_incremental_updates();
    cs.evaluator().bump_sv_defuse_incremental_updates();
    cs.evaluator().bump_sv_defuse_incremental_updates();
    cs.evaluator().bump_sv_defuse_incremental_updates();
    cs.evaluator().bump_sv_defuse_incremental_updates();
    const auto i1 = incr(cs);
    std::println("  incremental-updates: {} -> {}", i0, i1);
    CHECK(i1 == i0 + 5, "incremental-updates bumps by exactly 5");
}

static void run_ac5_sum(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: defuse-events-total == sum ---");
    const auto n = nested(cs);
    const auto cr = cross(cs);
    const auto i = incr(cs);
    const auto t = events_total(cs);
    std::println("  nested={} + cross={} + incremental={} = sum {} (primitive total {})", n, cr, i,
                 n + cr + i, t);
    CHECK(t == n + cr + i, "defuse-events-total == sum of 3 per-counters");
}

static void run_ac6_multi_round(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC6: multi-round simulate monotonic ---");
    const auto t_before = events_total(cs);
    // Simulate 20 nested modport discoveries + 15 cross-refs +
    // 10 incremental rebuilds (= 45 total events).
    for (int round = 0; round < 20; ++round)
        cs.evaluator().bump_sv_defuse_nested_modports();
    for (int round = 0; round < 15; ++round)
        cs.evaluator().bump_sv_defuse_cross_refs();
    for (int round = 0; round < 10; ++round)
        cs.evaluator().bump_sv_defuse_incremental_updates();
    const auto t_after = events_total(cs);
    std::println("  defuse-events-total: {} -> {}", t_before, t_after);
    CHECK(t_after == t_before + 45, "multi-round adds exactly 45 to defuse-events-total");
    CHECK(nested(cs) >= 20, "nested-modports accum >= 20 after simulate");
    CHECK(cross(cs) >= 15, "cross-refs accum >= 15 after simulate");
    CHECK(incr(cs) >= 10, "incremental-updates accum >= 10 after simulate");
}

static void run_ac7_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC7: regression — adjacent SV primitives reachable ---");
    auto sv_sva = cs.eval("(query:sv-sva-structure-stats)");
    auto sv_iface = cs.eval("(query:sv-interface-structure-stats)");
    auto sv_hw = cs.eval("(query:hardware-backend-sv-stats)");
    CHECK(sv_sva && aura::compiler::types::is_hash(*sv_sva),
          "query:sv-sva-structure-stats (schema 694) regression [hash]");
    CHECK(sv_iface && aura::compiler::types::is_hash(*sv_iface),
          "query:sv-interface-structure-stats (schema 661) regression [hash]");
    CHECK(sv_hw && aura::compiler::types::is_hash(*sv_hw),
          "query:hardware-backend-sv-stats (schema 663) regression [hash]");
}

} // namespace aura_issue_664_detail

int aura_issue_664_sv_defuse_stats_run() {
    using namespace aura_issue_664_detail;

    {
        aura::compiler::CompilerService cs;
        run_ac1_schema(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac2_nested(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac3_cross(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac4_incremental(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac5_sum(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac6_multi_round(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac7_regression(cs);
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_664_sv_defuse_stats_run();
}
#endif
