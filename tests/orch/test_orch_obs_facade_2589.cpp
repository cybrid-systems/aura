// tests/orch/test_orch_obs_facade_2589.cpp
// @category: integration
// @reason: Issue #2589 — unify parallel_intend residual/reclaim metrics into
//          the `query:orch-module-stats` facade so agents/dashboards query
//          one surface for cancel-storm health. Source of truth stays
//          ParallelOrchStats (src/serve/parallel_orch.h); facade is a live
//          read — NO double-bookkeeping on OrchModuleStats.
//
//   AC1: After parallel Timeout residual path bumps g_parallel_orch_stats
//        atomics, query:orch-module-stats exposes parallel-join-drain-*
//        keys without a second primitive.
//   AC2: Orch-only residual path unchanged — g_orch_moduleStats.join_*
//        atomics are NOT touched by parallel-only bumps (no mirror).
//   AC3: Schema version bump + wired sentinel present:
//        schema-2589 / issue-2589 / orch-obs-facade-unified-2589=1 /
//        parallel-join-drain-source=0.
//   AC4: src/orch/README.md documents the unified facade (live-read table).
//
// Source-cite (issue #2589):
//   - src/serve/parallel_orch.h: ParallelOrchStats.join_drain_residual_total /
//     _reclaim_total / _us_total (lines 130/135/136) — source of truth.
//   - src/orch/agent_spawn.h: OrchModuleStats.join_drain_residual_total /
//     _reclaim_total / _still_running / _body_retired_total — orch-side,
//     NOT mirrored (parallel-only bumps must NOT bump these).
//   - src/compiler/evaluator_primitives_agent.cpp: query:orch-module-stats
//     facade keys parallel-join-drain-residual-total /
//     parallel-join-drain-residual-reclaim-total /
//     parallel-join-drain-us-total / parallel-join-drain-source /
//     orch-obs-facade-unified-2589 / schema-2589 / issue-2589 (#2589).
//   - src/orch/README.md: "Observability facade (Issue #2589)" section.
//   - tests/orch/test_orch_obs_facade_2589.cpp (this file).
//
// No docs/design/ per #1655 / #1485.

#include "test_harness.hpp"

#include "orch/agent_spawn.h"
#include "serve/parallel_orch.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::orch::g_orch_module_stats;
using aura::serve::parallel_orch::g_parallel_orch_stats;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:orch-module-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void reset_counters() {
    // Parallel side (source of truth) — preserve then restore below the test.
    auto& p = g_parallel_orch_stats;
    p.join_drain_residual_total.store(0, std::memory_order_relaxed);
    p.join_drain_residual_reclaim_total.store(0, std::memory_order_relaxed);
    p.join_drain_us_total.store(0, std::memory_order_relaxed);
    // Orch side (must NOT mirror parallel).
    auto& m = g_orch_module_stats;
    m.join_drain_residual_total.store(0, std::memory_order_relaxed);
    m.join_drain_residual_reclaim_total.store(0, std::memory_order_relaxed);
    m.join_drain_residual_still_running.store(0, std::memory_order_relaxed);
    m.join_drain_residual_body_retired_total.store(0, std::memory_order_relaxed);
}

} // namespace

int run_test_orch_obs_facade_2589() {
    std::println("=== Issue #2589: orch-module-stats facade (parallel residual) ===");

    // ── AC4: src/orch/README.md documents the unified facade ──
    {
        std::println("\n--- #2589 AC4: README facade section ---");
        const auto readme_src = read_file("src/orch/README.md");
        CHECK(readme_src.find("Observability facade") != std::string::npos,
              "AC4: README has 'Observability facade' section");
        CHECK(readme_src.find("parallel-join-drain-residual-total") != std::string::npos,
              "AC4: README lists parallel-join-drain-residual-total key");
        CHECK(readme_src.find("parallel-join-drain-residual-reclaim-total") != std::string::npos,
              "AC4: README lists parallel-join-drain-residual-reclaim-total key");
        CHECK(readme_src.find("parallel-join-drain-source") != std::string::npos,
              "AC4: README documents parallel-join-drain-source sentinel");
        CHECK(readme_src.find("no double-bookkeeping") != std::string::npos,
              "AC4: README documents 'no double-bookkeeping' (live read)");
        CHECK(readme_src.find("#2227") != std::string::npos,
              "AC4: README references #2227 (hard-reclaim shared protocol)");
    }

    CompilerService cs;

    // ── AC1 + AC3: facade exposes parallel keys + schema/sentinel ──
    {
        std::println("\n--- #2589 AC1 + AC3: facade wired, schema-2589 + sentinel ---");
        reset_counters();
        // Pre-bump: all parallel counters at 0 → facade reflects 0.
        CHECK(href(cs, "parallel-join-drain-residual-total") == 0,
              "AC1: facade parallel-join-drain-residual-total = 0 before bump");
        CHECK(href(cs, "parallel-join-drain-residual-reclaim-total") == 0,
              "AC1: facade parallel-join-drain-residual-reclaim-total = 0 before bump");
        CHECK(href(cs, "parallel-join-drain-us-total") == 0,
              "AC1: facade parallel-join-drain-us-total = 0 before bump");
        CHECK(href(cs, "orch-obs-facade-unified-2589") == 1,
              "AC3: orch-obs-facade-unified-2589 sentinel = 1");
        CHECK(href(cs, "schema-2589") == 2589, "AC3: schema-2589 present");
        CHECK(href(cs, "issue-2589") == 2589, "AC3: issue-2589 present");
        CHECK(href(cs, "parallel-join-drain-source") == 0,
              "AC3: parallel-join-drain-source = 0 (ParallelOrchStats)");

        // Simulate parallel Timeout residual path bumps (#2227 hard reclaim).
        constexpr std::uint64_t kResidual = 7;
        constexpr std::uint64_t kReclaim = 5;
        constexpr std::uint64_t kDrainUs = 4321;
        auto& p = g_parallel_orch_stats;
        p.join_drain_residual_total.fetch_add(kResidual, std::memory_order_relaxed);
        p.join_drain_residual_reclaim_total.fetch_add(kReclaim, std::memory_order_relaxed);
        p.join_drain_us_total.fetch_add(kDrainUs, std::memory_order_relaxed);

        // Facade reflects the live ParallelOrchStats reads.
        CHECK(href(cs, "parallel-join-drain-residual-total") ==
                  static_cast<std::int64_t>(kResidual),
              "AC1: facade parallel-join-drain-residual-total = kResidual");
        CHECK(href(cs, "parallel-join-drain-residual-reclaim-total") ==
                  static_cast<std::int64_t>(kReclaim),
              "AC1: facade parallel-join-drain-residual-reclaim-total = kReclaim");
        CHECK(href(cs, "parallel-join-drain-us-total") == static_cast<std::int64_t>(kDrainUs),
              "AC1: facade parallel-join-drain-us-total = kDrainUs");
    }

    // ── AC2: Orch-only path unchanged — no double-count on agent join ──
    {
        std::println("\n--- #2589 AC2: orch-only path unchanged (no mirror) ---");
        reset_counters();
        // Snapshot OrchModuleStats orch-side residual/reclaim atomics.
        const auto before_residual =
            g_orch_module_stats.join_drain_residual_total.load(std::memory_order_relaxed);
        const auto before_reclaim =
            g_orch_module_stats.join_drain_residual_reclaim_total.load(std::memory_order_relaxed);
        const auto before_still =
            g_orch_module_stats.join_drain_residual_still_running.load(std::memory_order_relaxed);
        const auto before_retired = g_orch_module_stats.join_drain_residual_body_retired_total.load(
            std::memory_order_relaxed);
        // Bump ONLY parallel-side (no orch-side bump).
        constexpr std::uint64_t kParallelOnly = 11;
        g_parallel_orch_stats.join_drain_residual_total.fetch_add(kParallelOnly,
                                                                  std::memory_order_relaxed);
        g_parallel_orch_stats.join_drain_residual_reclaim_total.fetch_add(
            kParallelOnly, std::memory_order_relaxed);
        // Orch-side counters must be unchanged (no mirror atomics).
        CHECK(g_orch_module_stats.join_drain_residual_total.load(std::memory_order_relaxed) ==
                  before_residual,
              "AC2: OrchModuleStats.join_drain_residual_total NOT bumped by parallel-only");
        CHECK(g_orch_module_stats.join_drain_residual_reclaim_total.load(
                  std::memory_order_relaxed) == before_reclaim,
              "AC2: OrchModuleStats.join_drain_residual_reclaim_total NOT bumped by parallel-only");
        CHECK(g_orch_module_stats.join_drain_residual_still_running.load(
                  std::memory_order_relaxed) == before_still,
              "AC2: OrchModuleStats.join_drain_residual_still_running unchanged");
        CHECK(g_orch_module_stats.join_drain_residual_body_retired_total.load(
                  std::memory_order_relaxed) == before_retired,
              "AC2: OrchModuleStats.join_drain_residual_body_retired_total unchanged");
        // But the facade DOES reflect the parallel bumps (AC1 cross-check).
        CHECK(href(cs, "parallel-join-drain-residual-total") ==
                  static_cast<std::int64_t>(kParallelOnly),
              "AC1 cross-check: facade still reads ParallelOrchStats after orch-side baseline");

        reset_counters();
    }

    // ── #2636 AC1 + AC2 + AC5: body-age + force-safepoint opt-in facade keys ──
    // 2636 AC5 — facade keys + schema/sentinel/wired flags (linter marker).
    {
        std::println("\n--- #2636 AC1+AC2+AC5: body-age + env-opt-in facade ---");
        // Snapshot Fiber process-wide counters (delta-based to avoid touching
        // Fiber statics from this file — Fiber statics reset lives in
        // test_residual_force_safepoint_2533.cpp unit test).
        const auto age_max_before = aura::serve::Fiber::join_drain_residual_body_age_ms_max();
        const auto age_sum_before = aura::serve::Fiber::join_drain_residual_body_age_ms_sum();
        const auto age_samples_before = aura::serve::Fiber::join_drain_residual_body_age_samples();
        const auto fso_before = aura::serve::Fiber::force_safepoint_on_orphan_total();

        // AC5: query keys + schema/sentinel/wired flags present.
        CHECK(href(cs, "join-drain-residual-body-age-ms-max") ==
                  static_cast<std::int64_t>(age_max_before),
              "AC5: facade exposes join-drain-residual-body-age-ms-max");
        CHECK(href(cs, "join-drain-residual-body-age-ms-sum") ==
                  static_cast<std::int64_t>(age_sum_before),
              "AC5: facade exposes join-drain-residual-body-age-ms-sum");
        CHECK(href(cs, "join-drain-residual-body-age-samples") ==
                  static_cast<std::int64_t>(age_samples_before),
              "AC5: facade exposes join-drain-residual-body-age-samples");
        CHECK(href(cs, "force-safepoint-on-orphan-total") == static_cast<std::int64_t>(fso_before),
              "AC5: facade exposes force-safepoint-on-orphan-total");
        CHECK(href(cs, "force-safepoint-on-orphan-enabled") == 1,
              "AC5/AC3: default env = ON (preserves #2533 production behavior)");
        CHECK(href(cs, "schema-2636") == 2636, "AC5: schema-2636 present");
        CHECK(href(cs, "issue-2636") == 2636, "AC5: issue-2636 present");
        CHECK(href(cs, "residual-body-age-wired") == 1,
              "AC5: residual-body-age-wired sentinel = 1");
        CHECK(href(cs, "force-safepoint-on-orphan-wired") == 1,
              "AC5: force-safepoint-on-orphan-wired sentinel = 1");
    }

    std::println("\n=== #2589+#2636: {}/{} checks passed ===", g_passed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_orch_obs_facade_2589();
}
#endif
