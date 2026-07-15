// @category: integration
// @reason: Issue #665 — SV stability observability (P1 EDA-SV
//  scalability). Ships (query:sv-stability-stats, schema 665) +
//  3 CompilerMetrics atomics (sv_dirty_traversal_depth_total,
//  sv_generation_wrap_total, sv_stable_ref_invalidation_total) +
//  3 bump helpers + 3 accessors. The production wiring for Actions
//  #1 (early-exit / special-case SV tags in mark_dirty_upward) +
//  #2 (compact_nodes / restore subtree-gen scope for SV) is the
//  FOLLOW-UP scope; this commit ships the observability surface
//  + regression net.
//
//  Non-duplicative with #641 StableRef cross-fiber provenance,
//  #368/#392 generation fix, #336 mark_dirty_upward fast-path,
//  #642 arena, #664 SV DefUse.
//
//   - AC1:  query:sv-stability-stats reachable (schema 665)
//   - AC2:  dirty-traversal-depth bumps on
//           bump_sv_dirty_traversal_depth (with depth-of-N values)
//   - AC3:  generation-wrap-sv bumps on
//           bump_sv_generation_wrap
//   - AC4:  stable-ref-invalidation-sv bumps on
//           bump_sv_stable_ref_invalidation
//   - AC5:  stability-events-total == sum of 3 per-counter fields
//   - AC6:  multi-round simulation (50+30+20 = 100 events) +
//           multi-depth stress (track depth sum to mirror multi-level
//           SV mutate)
//   - AC7:  regression — adjacent SV primitives (sv-interface 661,
//           sv-sva 694, sv-defuse 664, hardware-backend-sv 663)
//           all reachable from same CompilerService (cross-feature)

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_665_detail {
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
        cs.eval(std::format("(hash-ref (engine:metrics \"query:sv-stability-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::int64_t depth(aura::compiler::CompilerService& cs) {
    return stat_int(cs, "dirty-traversal-depth");
}
static std::int64_t wrap(aura::compiler::CompilerService& cs) {
    return stat_int(cs, "generation-wrap-sv");
}
static std::int64_t inval(aura::compiler::CompilerService& cs) {
    return stat_int(cs, "stable-ref-invalidation-sv");
}
static std::int64_t events_total(aura::compiler::CompilerService& cs) {
    return stat_int(cs, "stability-events-total");
}

static void run_ac1_schema(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: query:sv-stability-stats (schema 665) ---");
    auto h = cs.eval("(engine:metrics \"query:sv-stability-stats\")");
    CHECK(h && aura::compiler::types::is_hash(*h), "sv-stability-stats returns hash");
    CHECK(stat_int(cs, "schema") == 665, "schema == 665");
    const auto d = depth(cs);
    const auto w = wrap(cs);
    const auto i = inval(cs);
    const auto t = events_total(cs);
    std::println("  baseline: depth={}, wrap={}, inval={}, total={}", d, w, i, t);
    CHECK(d >= 0, "dirty-traversal-depth non-negative");
    CHECK(w >= 0, "generation-wrap-sv non-negative");
    CHECK(i >= 0, "stable-ref-invalidation-sv non-negative");
    CHECK(t >= 0, "stability-events-total non-negative");
}

static void run_ac2_depth(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: dirty-traversal-depth bumps with depth values ---");
    const auto d0 = depth(cs);
    // Simulate 5 mark_dirty_upward calls at depths 1, 2, 3, 4, 5
    // (typical SV hierarchy depths: interface root → modport → port
    // → annotation → leaf).
    cs.evaluator().bump_sv_dirty_traversal_depth(1);
    cs.evaluator().bump_sv_dirty_traversal_depth(2);
    cs.evaluator().bump_sv_dirty_traversal_depth(3);
    cs.evaluator().bump_sv_dirty_traversal_depth(4);
    cs.evaluator().bump_sv_dirty_traversal_depth(5);
    const auto d1 = depth(cs);
    std::println("  depth: {} -> {} (expect +15 = 1+2+3+4+5)", d0, d1);
    CHECK(d1 == d0 + 15, "dirty-traversal-depth bumps by exactly 15 (sum of 1..5)");
}

static void run_ac3_wrap(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: generation-wrap-sv bumps ---");
    const auto w0 = wrap(cs);
    cs.evaluator().bump_sv_generation_wrap();
    cs.evaluator().bump_sv_generation_wrap();
    cs.evaluator().bump_sv_generation_wrap();
    const auto w1 = wrap(cs);
    std::println("  wrap: {} -> {}", w0, w1);
    CHECK(w1 == w0 + 3, "generation-wrap-sv bumps by exactly 3");
}

static void run_ac4_invalidation(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: stable-ref-invalidation-sv bumps ---");
    const auto i0 = inval(cs);
    cs.evaluator().bump_sv_stable_ref_invalidation();
    cs.evaluator().bump_sv_stable_ref_invalidation();
    cs.evaluator().bump_sv_stable_ref_invalidation();
    cs.evaluator().bump_sv_stable_ref_invalidation();
    const auto i1 = inval(cs);
    std::println("  inval: {} -> {}", i0, i1);
    CHECK(i1 == i0 + 4, "stable-ref-invalidation-sv bumps by exactly 4");
}

static void run_ac5_sum(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: stability-events-total == sum ---");
    const auto d = depth(cs);
    const auto w = wrap(cs);
    const auto i = inval(cs);
    const auto t = events_total(cs);
    std::println("  depth={} + wrap={} + inval={} = sum {} (primitive total {})", d, w, i,
                 d + w + i, t);
    CHECK(t == d + w + i, "stability-events-total == sum of 3 per-counters");
}

static void run_ac6_multi_round(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC6: multi-round simulation with multi-depth stress ---");
    const auto t_before = events_total(cs);
    const auto d_before = depth(cs);
    // Simulate a 5-tier deep stress scenario:
    //   - 50 dirty traversals at varying depths (avg depth ~5 = 250)
    //   - 30 generation wrap hits
    //   - 20 stable-ref invalidations
    for (int round = 0; round < 50; ++round)
        cs.evaluator().bump_sv_dirty_traversal_depth(5); // 250 depth total
    for (int round = 0; round < 30; ++round)
        cs.evaluator().bump_sv_generation_wrap();
    for (int round = 0; round < 20; ++round)
        cs.evaluator().bump_sv_stable_ref_invalidation();
    const auto t_after = events_total(cs);
    const auto d_after = depth(cs);
    std::println("  total: {} -> {} (expect +300)", t_before, t_after);
    std::println("  depth: {} -> {} (expect +250)", d_before, d_after);
    CHECK(t_after == t_before + 300, "events-total grew by exactly 300 (250+30+20)");
    CHECK(d_after == d_before + 250, "depth grew by exactly 250 (50×5)");
    CHECK(wrap(cs) >= 30, "generation-wrap-sv accum >= 30 after simulate");
    CHECK(inval(cs) >= 20, "stable-ref-invalidation-sv accum >= 20 after simulate");
}

static void run_ac7_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC7: regression — adjacent SV primitives reachable ---");
    auto sv_iface = cs.eval("(engine:metrics \"query:sv-interface-structure-stats\")");
    auto sv_sva = cs.eval("(engine:metrics \"query:sv-sva-structure-stats\")");
    auto sv_defuse = cs.eval("(engine:metrics \"query:sv-defuse-stats\")");
    auto sv_hw = cs.eval("(engine:metrics \"query:hardware-backend-sv-stats\")");
    CHECK(sv_iface && aura::compiler::types::is_hash(*sv_iface),
          "query:sv-interface-structure-stats (schema 661) regression [hash]");
    CHECK(sv_sva && aura::compiler::types::is_hash(*sv_sva),
          "query:sv-sva-structure-stats (schema 694) regression [hash]");
    CHECK(sv_defuse && aura::compiler::types::is_hash(*sv_defuse),
          "query:sv-defuse-stats (schema 664) regression [hash]");
    CHECK(sv_hw && aura::compiler::types::is_hash(*sv_hw),
          "query:hardware-backend-sv-stats (schema 663) regression [hash]");
}

} // namespace aura_issue_665_detail

int aura_issue_665_sv_stability_stats_run() {
    using namespace aura_issue_665_detail;

    {
        aura::compiler::CompilerService cs;
        run_ac1_schema(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac2_depth(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac3_wrap(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac4_invalidation(cs);
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
    return aura_issue_665_sv_stability_stats_run();
}
#endif
