// tests/orch/test_orch_obs_facade.cpp
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
//   - tests/orch/test_orch_obs_facade.cpp (this file).
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

int run_test_orch_obs_facade() {
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
        // test_residual_force_safepoint.cpp unit test).
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

    // ── #2884: C++ helper agent_send_safe unifies #2663 / #2848 contract ──
    {
        std::println("\n--- #2884 AC1+AC5: helper exists + schema-2884 in posture prim ---");
        const auto agent_spawn_src = read_file("src/orch/agent_spawn.h");
        const auto posture_prim_src = read_file("src/compiler/evaluator_primitives_agent.cpp");

        // AC1: helper defined in agent_spawn.h (unifies C++ / language path).
        CHECK(agent_spawn_src.find("agent_send_safe") != std::string::npos,
              "2884 AC1: agent_send_safe helper exists in agent_spawn.h");
        CHECK(agent_spawn_src.find("Evaluator*") != std::string::npos,
              "2884 AC1: agent_send_safe takes optional Evaluator* parameter (opaque void*)");
        CHECK(agent_spawn_src.find("void* ev") != std::string::npos,
              "2884 AC1: agent_send_safe uses void* ABI (no evaluator module import)");
        CHECK(agent_spawn_src.find("aura_orch_agent_send_handoff") != std::string::npos,
              "2884 AC1: handoff via extern C hook (no module import in orch header)");
        CHECK(agent_spawn_src.find("import aura.compiler.evaluator;") == std::string::npos,
              "2884 AC1: agent_spawn.h must not import evaluator (asan ddi / module already "
              "imported)");
        CHECK(agent_spawn_src.find("Status::HandoffRequired") != std::string::npos,
              "2884 AC1: agent_send_safe returns PushStatus::HandoffRequired on handoff fail");
        CHECK(agent_spawn_src.find("agent_send_safe_handoff_required_total") != std::string::npos,
              "2884 AC1: agent_send_safe bumps agent_send_safe_handoff_required_total");
        CHECK(agent_spawn_src.find("agent_send_safe_total") != std::string::npos,
              "2884 AC1: agent_send_safe bumps agent_send_safe_total");

        // AC5: schema-2884 + new counters exposed via query:orch-module-stats.
        CHECK(posture_prim_src.find("schema-2884") != std::string::npos,
              "2884 AC5: schema-2884 inserted in posture prim");
        CHECK(posture_prim_src.find("issue-2884") != std::string::npos,
              "2884 AC5: issue-2884 inserted in posture prim");
        CHECK(posture_prim_src.find("agent-send-safe-wired") != std::string::npos,
              "2884 AC5: agent-send-safe-wired sentinel inserted");
        CHECK(posture_prim_src.find("agent-send-safe-total") != std::string::npos,
              "2884 AC5: agent-send-safe-total counter exposed");
        CHECK(posture_prim_src.find("agent-send-safe-handoff-required-total") != std::string::npos,
              "2884 AC5: agent-send-safe-handoff-required-total exposed");

        // Live runtime counters exposed via hash (query:orch-module-stats).
        CHECK(href(cs, "schema-2884") == 2884, "2884 AC5: schema-2884 == 2884 (live posture prim)");
        CHECK(href(cs, "issue-2884") == 2884, "2884 AC5: issue-2884 == 2884 (live posture prim)");
        CHECK(href(cs, "agent-send-safe-wired") == 1,
              "2884 AC5: agent-send-safe-wired sentinel = 1");
        CHECK(href(cs, "agent-send-safe-total") >= 0,
              "2884 AC5: agent-send-safe-total counter queryable (initial 0)");
        CHECK(href(cs, "agent-send-safe-handoff-required-total") >= 0,
              "2884 AC5: agent-send-safe-handoff-required-total queryable (initial 0)");

        // AC4: language (orch:agent-send) behaviour preserved (#2848 source-cite
        // still present — no regression in the auto-handoff path).
        CHECK(posture_prim_src.find("schema-2848") != std::string::npos,
              "2884 AC4: schema-2848 still wired (language auto-handoff preserved)");
        CHECK(posture_prim_src.find("agent-send-auto-handoff-wired") != std::string::npos,
              "2884 AC4: agent-send-auto-handoff-wired still present");
    }

    // ── #2884 AC2/AC3: PushStatus::HandoffRequired distinct from Closed ──
    {
        std::println("\n--- #2884 AC2/AC3: HandoffRequired distinct from Closed ---");
        const auto mf_mailbox_src = read_file("src/serve/multi_fiber_mailbox.h");
        // AC3: Closed (=2) still gates raw push (defense in depth per #2663).
        CHECK(mf_mailbox_src.find("Closed = 2") != std::string::npos,
              "2884 AC3: PushStatus::Closed = 2 still defined (raw push gate intact)");
        // AC2/AC3: HandoffRequired (=3) is the distinct typed fail for the safe helper.
        CHECK(mf_mailbox_src.find("HandoffRequired = 3") != std::string::npos,
              "2884 AC2/AC3: PushStatus::HandoffRequired = 3 distinct from Closed");
        CHECK(mf_mailbox_src.find("never silent Closed") != std::string::npos,
              "2884 AC2/AC3: HandoffRequired source-cite documents no-ambiguous-Closed contract");
    }

    // ── #2884 AC6: source-cite + no invent + no docs/design/ ──
    {
        std::println("\n--- #2884 AC6: source-cite + no invent + no docs/design/ ---");
        const auto agent_spawn_src = read_file("src/orch/agent_spawn.h");
        const auto mf_mailbox_src = read_file("src/serve/multi_fiber_mailbox.h");
        const auto posture_prim_src = read_file("src/compiler/evaluator_primitives_agent.cpp");

        // #2884 source-cite in all three lineage TUs.
        CHECK(agent_spawn_src.find("Issue #2884") != std::string::npos,
              "2884 AC6: agent_spawn.h cites Issue #2884");
        CHECK(mf_mailbox_src.find("Issue #2884") != std::string::npos,
              "2884 AC6: multi_fiber_mailbox.h cites Issue #2884 (HandoffRequired enum)");
        CHECK(posture_prim_src.find("schema-2884") != std::string::npos,
              "2884 AC6: evaluator_primitives_agent.cpp surfaces schema-2884");

        // No new test_issue_2884.cpp (per #81967).
        std::ifstream invent_c("tests/core/test_issue_2884.cpp");
        if (!invent_c.good())
            invent_c.open("../tests/core/test_issue_2884.cpp");
        CHECK(!invent_c.good(),
              "2884 AC6: no tests/core/test_issue_2884.cpp (forbidden per #81967)");
        std::ifstream invent_op("tests/orch/test_issue_2884.cpp");
        if (!invent_op.good())
            invent_op.open("../tests/orch/test_issue_2884.cpp");
        CHECK(!invent_op.good(),
              "2884 AC6: no tests/orch/test_issue_2884.cpp (forbidden per #81967)");

        // No docs/design/2884-* (per #1655).
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("2884-") == std::string::npos,
                      std::string("2884 AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    std::println("\n=== #2589+#2636+2884: {}/{} checks passed ===", g_passed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_orch_obs_facade();
}
#endif
