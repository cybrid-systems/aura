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
// Issue #3733 — query:orch-module-stats overflow contract + missing live
// OrchModuleStats atomics on the Agent hash (append only; no new query:*).
//   AC1: aura_query_hash_set_force_cap → hash non-void + overflow sentinel
//        (never silent drop).
//   AC2: production tenant-required deny → spawn-tenant-required-total ≥ 1
//        on engine:metrics "query:orch-module-stats" (not C++ atomic load).
//   AC3: existing keys (agents-spawned, schema sentinels) unchanged.
//   AC4: Soft: same hash, no extra atomics. No test_issue_N.cpp.
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

#include "compiler/agent_name_table.h"
#include "compiler/typed_mutation_audit.h"
#include "core/provenance_tracker.hh"
#include "core/resource_quota.hh"
#include "core/sandbox.hh"
#include "orch/agent_spawn.h"
#include "serve/parallel_orch.h"
#include "serve/scheduler.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::orch::g_orch_module_stats;
using aura::serve::parallel_orch::g_parallel_orch_stats;
using aura::test::g_failed;
using aura::test::g_passed;

extern "C" void aura_query_hash_set_force_cap(std::uint64_t);

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

    // ── #3013: raw agent_send unstamped held_ref → HandoffRequired ──
    {
        std::println("\n--- #3013 AC1: agent_send unstamped held_ref is HandoffRequired ---");
        using aura::orch::agent_send;
        using aura::orch::agent_send_safe;
        using aura::orch::AgentHandle;
        using aura::orch::stamp_mail_message_handoff_completed;
        using aura::serve::mf_mailbox::MailMessage;
        using aura::serve::mf_mailbox::MultiFiberMailbox;
        using aura::serve::mf_mailbox::PushStatus;
        AgentHandle h;
        h.ok = true;
        h.mailbox = std::make_shared<MultiFiberMailbox>(/*high_water=*/64);
        MailMessage raw;
        raw.payload = "held-no-stamp";
        raw.held_ref_token = 42;
        raw.handoff_completed = false;
        const auto before = g_orch_module_stats.agent_send_safe_handoff_required_total.load(
            std::memory_order_relaxed);
        const auto st = agent_send(h, std::move(raw));
        CHECK(st == PushStatus::HandoffRequired,
              "3013 AC1: raw agent_send unstamped held_ref → HandoffRequired");
        CHECK(g_orch_module_stats.agent_send_safe_handoff_required_total.load(
                  std::memory_order_relaxed) >= before + 1,
              "3013 AC1: reuses agent_send_safe_handoff_required_total");
        MailMessage raw2;
        raw2.payload = "held-no-stamp-safe";
        raw2.held_ref_token = 43;
        raw2.handoff_completed = false;
        const auto st_safe = agent_send_safe(h, std::move(raw2), /*ev=*/nullptr);
        CHECK(st_safe == PushStatus::HandoffRequired,
              "3013 AC1: agent_send_safe(ev=null) unstamped also HandoffRequired");

        std::println("\n--- #3013 AC2: no token / stamped stay zero-cost Ok ---");
        MailMessage plain;
        plain.payload = "plain";
        CHECK(agent_send(h, std::move(plain)) == PushStatus::Ok,
              "3013 AC2: no held_ref_token still Ok");
        MailMessage stamped;
        stamped.payload = "stamped";
        stamp_mail_message_handoff_completed(stamped, 44);
        CHECK(agent_send(h, std::move(stamped)) == PushStatus::Ok,
              "3013 AC2: already-stamped still Ok");

        std::println("\n--- #3013 AC3 / #3212: mailbox push unstamped is HandoffRequired ---");
        MailMessage gate;
        gate.payload = "direct-push";
        gate.held_ref_token = 45;
        gate.handoff_completed = false;
        CHECK(h.mailbox->push(std::move(gate)) == PushStatus::HandoffRequired,
              "3013 AC3/#3212: direct mb.push unstamped → HandoffRequired");

        std::println("\n--- #3013 AC4/AC5: schema + prefer agent_send_safe ---");
        const auto spawn = read_file("src/orch/agent_spawn.h");
        const auto agent = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto mb = read_file("src/serve/multi_fiber_mailbox.h");
        CHECK(spawn.find("Issue #3013") != std::string::npos,
              "3013 AC4: agent_spawn.h cites #3013");
        CHECK(spawn.find("prefer agent_send_safe") != std::string::npos,
              "3013 AC4: deprecation / prefer-safe comment");
        CHECK(agent.find("schema-3013") != std::string::npos, "3013 AC5: schema-3013");
        CHECK(agent.find("agent-send-handoff-required-wired") != std::string::npos,
              "3013 AC5: wired key");
        CHECK(mb.find("Issue #2884 / #3013") != std::string::npos ||
                  mb.find("#3013") != std::string::npos,
              "3013 AC5: mailbox enum cites #3013");
        CHECK(href(cs, "schema-3013") == 3013, "3013 AC5: live schema-3013");
        CHECK(href(cs, "agent-send-handoff-required-wired") == 1, "3013 AC5: live wired sentinel");

        std::println("\n--- #3013 AC6: extend suite + no invent + no docs/design/ ---");
        const auto t = read_file("tests/orch/test_orch_obs_facade.cpp");
        CHECK(t.find("#3013 AC1") != std::string::npos, "3013 AC6: this suite cites #3013");
        const auto build = read_file("build.py");
        CHECK(build.find("check_agent_send_handoff_required_3013") != std::string::npos,
              "3013 AC6: build.py wires #3013 linter");
        std::ifstream invent("tests/orch/test_issue_3013.cpp");
        if (!invent.good())
            invent.open("../tests/orch/test_issue_3013.cpp");
        CHECK(!invent.good(), "3013 AC6: no test_issue_3013.cpp per #81967");
        CHECK(read_file("docs/design/3013-agent-send-handoff-required.md").empty(),
              "3013 AC6: no docs/design/3013-* per #1655");
    }

    // ── #3212: dual-track mailbox push / agent_send HandoffRequired ──
    {
        std::println("\n--- #3212 AC1: unstamped held_ref is HandoffRequired on both paths ---");
        using aura::orch::agent_send;
        using aura::orch::AgentHandle;
        using aura::serve::mf_mailbox::g_mf_mailbox_stats;
        using aura::serve::mf_mailbox::MailMessage;
        using aura::serve::mf_mailbox::MultiFiberMailbox;
        using aura::serve::mf_mailbox::PushStatus;
        AgentHandle h;
        h.ok = true;
        h.mailbox = std::make_shared<MultiFiberMailbox>(/*high_water=*/64);
        const auto reject0 =
            g_mf_mailbox_stats.handoff_reject_total.load(std::memory_order_relaxed);

        MailMessage via_send;
        via_send.payload = "send-unstamped";
        via_send.held_ref_token = 101;
        via_send.handoff_completed = false;
        CHECK(agent_send(h, std::move(via_send)) == PushStatus::HandoffRequired,
              "3212 AC1: agent_send unstamped → HandoffRequired");

        MailMessage via_push;
        via_push.payload = "push-unstamped";
        via_push.held_ref_token = 102;
        via_push.handoff_completed = false;
        CHECK(h.mailbox->push(std::move(via_push)) == PushStatus::HandoffRequired,
              "3212 AC1: mailbox->push unstamped → HandoffRequired");
        CHECK(g_mf_mailbox_stats.handoff_reject_total.load(std::memory_order_relaxed) >=
                  reject0 + 1,
              "3212 AC1: mailbox still bumps handoff_reject_total");

        MailMessage via_fanout;
        via_fanout.payload = "fanout-unstamped";
        via_fanout.held_ref_token = 103;
        via_fanout.handoff_completed = false;
        CHECK(h.mailbox->broadcast_fanout(via_fanout) == PushStatus::HandoffRequired,
              "3212 AC1: broadcast_fanout unstamped → HandoffRequired");

        std::println("\n--- #3212 AC2: true closed mailbox still Closed ---");
        h.mailbox->close();
        MailMessage after_close;
        after_close.payload = "after-close";
        CHECK(h.mailbox->push(std::move(after_close)) == PushStatus::Closed,
              "3212 AC2: true closed mailbox still Closed");
        MailMessage closed_held;
        closed_held.payload = "closed-held";
        closed_held.held_ref_token = 104;
        closed_held.handoff_completed = false;
        // Unstamped gate fires before closed_ load — still HandoffRequired
        // (typed miss, not "mailbox closed").
        CHECK(h.mailbox->push(std::move(closed_held)) == PushStatus::HandoffRequired,
              "3212 AC2: unstamped on closed mailbox is HandoffRequired (gate first)");

        std::println("\n--- #3212 AC3: no held_ref stays Ok (zero extra) ---");
        AgentHandle h2;
        h2.ok = true;
        h2.mailbox = std::make_shared<MultiFiberMailbox>(/*high_water=*/64);
        MailMessage plain;
        plain.payload = "plain-3212";
        CHECK(h2.mailbox->push(std::move(plain)) == PushStatus::Ok,
              "3212 AC3: no held_ref_token still Ok");
        CHECK(agent_send(h2, MailMessage{.payload = "plain-send"}) == PushStatus::Ok,
              "3212 AC3: agent_send no token still Ok");

        std::println("\n--- #3212 AC4/AC5: schema + source-cite ---");
        const auto mb = read_file("src/serve/multi_fiber_mailbox.h");
        const auto spawn = read_file("src/orch/agent_spawn.h");
        const auto agent = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(mb.find("return PushStatus::HandoffRequired") != std::string::npos,
              "3212 AC4: mailbox push/fanout return HandoffRequired");
        CHECK(mb.find("Issue #3212") != std::string::npos, "3212 AC4: mailbox cites #3212");
        CHECK(spawn.find("Issue #3013 / #3212") != std::string::npos,
              "3212 AC4: agent_send cites #3212");
        CHECK(agent.find("schema-3212") != std::string::npos, "3212 AC5: schema-3212");
        CHECK(agent.find("mailbox-handoff-required-wired") != std::string::npos,
              "3212 AC5: wired key");
        CHECK(href(cs, "schema-3212") == 3212, "3212 AC5: live schema-3212");
        CHECK(href(cs, "mailbox-handoff-required-wired") == 1, "3212 AC5: live wired sentinel");
        CHECK(href(cs, "schema-3013") == 3013, "3212 AC5: schema-3013 preserved");

        std::println("\n--- #3212 AC6: extend suite + no invent ---");
        const auto build = read_file("build.py");
        CHECK(build.find("check_mailbox_handoff_dual_track_3212") != std::string::npos,
              "3212 AC6: build.py wires linter");
        CHECK(read_file("docs/design/3212-mailbox-handoff-dual-track.md").empty(),
              "3212 AC6: no docs/design/3212-* per #1655");
        CHECK(read_file("tests/orch/test_issue_3212.cpp").empty(),
              "3212 AC6: no test_issue_3212.cpp per #81967");
    }

    {
        std::println("\n--- #3251: unified deny-class surface ---");
        CHECK(aura::orch::kAgentDenyClassIssue == 3251, "3251: stamp");
        CHECK(href(cs, "schema-3251") == 3251, "3251: query schema-3251");
        CHECK(href(cs, "agent-deny-class-wired") == 1, "3251: wired");
        CHECK(std::string_view(
                  aura::orch::agent_deny_class_name(aura::orch::AgentDenyClass::Quota)) == "quota",
              "3251: quota name");
        CHECK(std::string_view(aura::orch::agent_deny_class_name(
                  aura::orch::AgentDenyClass::Handoff)) == "handoff",
              "3251: handoff name");
        const auto spawn_src = read_file("src/orch/agent_spawn.h");
        const auto agent = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto fib = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        CHECK(spawn_src.find("classify_agent_deny") != std::string::npos, "3251: classify helper");
        CHECK(agent.find("deny-class") != std::string::npos, "3251: Aura deny-class");
        CHECK(fib.find("admit_security_schedule") != std::string::npos, "3251: body schedule-gate");
        CHECK(read_file("src/orch/README.md").find("deny-class") != std::string::npos,
              "3251: README deny-class");
        CHECK(read_file("docs/design/3251-agent-deny-class.md").empty(),
              "3251: no docs/design/ per #1655");
        CHECK(read_file("tests/orch/test_issue_3251.cpp").empty(),
              "3251: no test_issue_3251.cpp per #81967");

        aura::compiler::typed_audit::apply_production_audit_defaults();
        (void)cs.eval(R"((orch:spawn-agent "3251-closed" (lambda () 0) :attach-mailbox #f))");
        const auto sch = cs.eval(R"((hash-ref (orch:agent-send "3251-closed" "x") "schema-3251"))");
        CHECK(sch && is_int(*sch) && as_int(*sch) == 3251,
              "3251: production send Closed hash schema-3251");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        (void)cs.eval(R"((orch:spawn-agent "3251-soft" (lambda () 0) :attach-mailbox #f))");
        const auto sch_s = cs.eval(R"((hash-ref (orch:agent-send "3251-soft" "x") "schema-3251"))");
        CHECK(!sch_s || !is_int(*sch_s), "3251: Soft send no deny-class intern");
    }

    // ── #3336: production C++ agent_send preference ──
    {
        using aura::orch::agent_send;
        using aura::orch::AgentHandle;
        using aura::serve::mf_mailbox::MailMessage;
        using aura::serve::mf_mailbox::MultiFiberMailbox;
        using aura::serve::mf_mailbox::PushStatus;
        std::println("\n--- #3336 AC1: production sites safe or annotated ---");
        CHECK(read_file("src/compiler/evaluator_primitives_agent.cpp").find("orch-raw-send-ok") !=
                  std::string::npos,
              "ac3336_1_production_sites_safe_or_annotated");
        CHECK(read_file("src/orch/agent_spawn.h").find("kAgentSendSafePreferenceIssue = 3336") !=
                  std::string::npos,
              "3336 AC1: issue stamp");

        std::println("\n--- #3336 AC2: zero-cost plain / stamped unchanged ---");
        AgentHandle h;
        h.ok = true;
        h.mailbox = std::make_shared<MultiFiberMailbox>(/*high_water=*/64);
        const auto raw0 =
            g_orch_module_stats.agent_send_raw_held_ref_total.load(std::memory_order_relaxed);
        CHECK(agent_send(h, MailMessage{.payload = "plain-3336"}) == PushStatus::Ok,
              "ac3336_2_zero_cost_plain_unchanged");
        MailMessage stamped;
        stamped.payload = "stamped-3336";
        aura::orch::stamp_mail_message_handoff_completed(stamped, 77);
        CHECK(agent_send(h, std::move(stamped)) == PushStatus::Ok, "3336 AC2: already-stamped Ok");
        CHECK(g_orch_module_stats.agent_send_raw_held_ref_total.load(std::memory_order_relaxed) ==
                  raw0,
              "ac3336_3_soft_quiet_no_raw_counter");

        std::println("\n--- #3336 AC4: unstamped still HandoffRequired ---");
        MailMessage raw;
        raw.payload = "unstamped-3336";
        raw.held_ref_token = 88;
        raw.handoff_completed = false;
        CHECK(agent_send(h, std::move(raw)) == PushStatus::HandoffRequired,
              "ac3336_4_unstamped_still_handoff_required");
        CHECK(g_orch_module_stats.agent_send_raw_held_ref_total.load(std::memory_order_relaxed) >=
                  raw0 + 1,
              "3336 AC4: raw_held_ref counter bumped on unstamped");

        std::println("\n--- #3336 AC5: schema + linter ---");
        CHECK(href(cs, "schema-3336") == aura::orch::kAgentSendSafePreferenceIssue,
              "3336 AC5: schema-3336");
        CHECK(href(cs, "agent-send-safe-preference-wired") == 1, "3336 AC5: wired");
        CHECK(href(cs, "agent-send-raw-held-ref-total") >= 0, "3336 AC5: raw-held-ref key");
        CHECK(href(cs, "schema-3013") == 3013, "3336 AC5: schema-3013 preserved");
        const auto lint =
            read_file("scripts/coverage/checks/check_agent_send_safe_preference_3336.py");
        CHECK(!lint.empty() && lint.find("Issue #3336") != std::string::npos,
              "ac3336_5_source_and_linter");
        const auto build = read_file("build.py");
        CHECK(build.find("check_agent_send_safe_preference_3336") != std::string::npos,
              "3336 AC5: build.py");
        CHECK(build.find("check_agent_send_handoff_required_3013") != std::string::npos,
              "3336 AC5: #3013 linter retained");
        CHECK(read_file("tests/orch/test_issue_3336.cpp").empty(),
              "3336 AC5: no test_issue_3336.cpp (#81967)");
        CHECK(read_file("docs/design/3336-agent-send-safe-preference.md").empty(),
              "3336 AC5: no docs/design/3336-* (#1655)");
    }

    // ── #3565: steal-cleared held_ref is not a successful recv ──
    {
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        using aura::orch::agent_recv;
        using aura::orch::agent_send;
        using aura::orch::AgentHandle;
        using aura::orch::stamp_mail_message_handoff_completed;
        using aura::serve::Fiber;
        using aura::serve::mf_mailbox::g_mf_mailbox_stats;
        using aura::serve::mf_mailbox::MailMessage;
        using aura::serve::mf_mailbox::MultiFiberMailbox;
        using aura::serve::mf_mailbox::PushStatus;

        std::println("\n--- #3565 AC1: production recv after stamp-clear is not success ---");
        apply_production_audit_defaults();
        AgentHandle h;
        h.ok = true;
        h.mailbox = std::make_shared<MultiFiberMailbox>(/*high_water=*/64);
        MailMessage stamped;
        stamped.payload = "stable-ref:9:1";
        stamp_mail_message_handoff_completed(stamped, 9);
        CHECK(agent_send(h, std::move(stamped)) == PushStatus::Ok, "3565 AC1: stamped push Ok");
        Fiber dummy([] {});
        h.mailbox->for_each_pending_held_ref_for_fiber(&dummy, [](auto& m) {
            if (m.handoff_completed) {
                m.handoff_completed = false;
                aura::serve::mf_mailbox::bump_held_ref_stale_after_steal();
            }
        });
        const auto recv0 = g_orch_module_stats.agents_recv.load(std::memory_order_relaxed);
        const auto reject0 =
            g_mf_mailbox_stats.handoff_reject_total.load(std::memory_order_relaxed);
        auto got = agent_recv(h, /*wait=*/false, /*timeout_ms=*/0);
        // Issue #3642: stale held_ref is no longer surfaced as a success —
        // nullopt for the C++ host; the stale signal rides the handle for
        // the Aura typed fail.
        CHECK(!got.has_value(), "3642 AC1: stale recv is nullopt (not a success)");
        CHECK(h.last_recv_stale_handoff, "3642 AC1: stale signal rides the handle");
        h.last_recv_stale_handoff = false;
        CHECK(g_orch_module_stats.agents_recv.load(std::memory_order_relaxed) == recv0,
              "3565 AC1: agents_recv not counted as success");
        CHECK(g_mf_mailbox_stats.handoff_reject_total.load(std::memory_order_relaxed) >=
                  reject0 + 1,
              "3565 AC1: handoff_reject_total bumped");
        MailMessage later;
        later.payload = "later-plain";
        CHECK(agent_send(h, std::move(later)) == PushStatus::Ok, "3565 AC1: later plain push");
        auto got2 = agent_recv(h, /*wait=*/false, /*timeout_ms=*/0);
        CHECK(got2.has_value() && got2->payload == "later-plain",
              "3565 AC1: later message not stuck behind stale held_ref");

        std::println("\n--- #3565 AC3: Soft still delivers ---");
        apply_dev_audit_defaults();
        AgentHandle hs;
        hs.ok = true;
        hs.mailbox = std::make_shared<MultiFiberMailbox>(/*high_water=*/64);
        MailMessage soft_stamped;
        soft_stamped.payload = "stable-ref:8:1";
        stamp_mail_message_handoff_completed(soft_stamped, 8);
        CHECK(agent_send(hs, std::move(soft_stamped)) == PushStatus::Ok, "3565 AC3: push");
        Fiber dummy2([] {});
        hs.mailbox->for_each_pending_held_ref_for_fiber(
            &dummy2, [](auto& m) { m.handoff_completed = false; });
        auto soft_got = agent_recv(hs, /*wait=*/false, /*timeout_ms=*/0);
        CHECK(soft_got.has_value() && soft_got->payload == "stable-ref:8:1",
              "3565 AC3: Soft delivers stale payload (#3111 AC3)");

        std::println("\n--- #3565 AC4: ordinary string recv unchanged ---");
        apply_production_audit_defaults();
        AgentHandle hp;
        hp.ok = true;
        hp.mailbox = std::make_shared<MultiFiberMailbox>(/*high_water=*/64);
        CHECK(agent_send(hp, MailMessage{.payload = "plain-3565"}) == PushStatus::Ok,
              "3565 AC4: plain send");
        auto plain = agent_recv(hp, /*wait=*/false, /*timeout_ms=*/0);
        CHECK(plain.has_value() && plain->payload == "plain-3565",
              "3565 AC4: no held_ref_token still delivers");

        std::println("\n--- #3565 AC2/AC5: Aura typed fail + no invent ---");
        const auto prim = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto mb = read_file("src/serve/multi_fiber_mailbox.h");
        const auto spawn = read_file("src/orch/agent_spawn.h");
        CHECK(prim.find("schema-3565") != std::string::npos, "3565 AC2: orch:agent-recv schema");
        CHECK(prim.find("handoff-required") != std::string::npos &&
                  prim.find("kRecvHeldRefAfterStealIssue") != std::string::npos,
              "3565 AC2: Aura handoff-required branch");
        CHECK(mb.find("maybe_clear_stale_held_ref_on_recv") != std::string::npos,
              "3565 AC5: mailbox recv gate");
        CHECK(spawn.find("kRecvHeldRefAfterStealIssue = 3565") != std::string::npos,
              "3565 AC5: issue constant");
        CHECK(read_file("tests/orch/test_issue_3565.cpp").empty() &&
                  read_file("tests/issues/test_issue_3565.cpp").empty(),
              "3565 AC5: no test_issue_3565.cpp");
        CHECK(read_file("docs/design/3565-recv-held-ref.md").empty(),
              "3565 AC5: no docs/design/3565-*");
        apply_dev_audit_defaults();
    }

    // ── #3642: stale held_ref consumes as nullopt (mailbox level + helper) ──
    {
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        using aura::orch::agent_recv;
        using aura::orch::agent_send;
        using aura::orch::AgentHandle;
        using aura::orch::stamp_mail_message_handoff_completed;
        using aura::serve::Fiber;
        using aura::serve::mf_mailbox::g_mf_mailbox_stats;
        using aura::serve::mf_mailbox::MailMessage;
        using aura::serve::mf_mailbox::MultiFiberMailbox;
        using aura::serve::mf_mailbox::PushStatus;

        std::println("\n--- #3642 AC1: raw mailbox recv/try_pop consume stale as empty ---");
        apply_production_audit_defaults();
        AgentHandle h3642;
        h3642.ok = true;
        auto mb = std::make_shared<MultiFiberMailbox>(/*high_water=*/64);
        h3642.mailbox = mb;
        MailMessage stamped3642;
        stamped3642.payload = "stable-ref:11:1";
        stamp_mail_message_handoff_completed(stamped3642, 11);
        CHECK(agent_send(h3642, std::move(stamped3642)) == PushStatus::Ok,
              "3642 AC1: stamped push Ok");
        Fiber dummy3642([] {});
        mb->for_each_pending_held_ref_for_fiber(&dummy3642, [](auto& m) {
            if (m.handoff_completed) {
                m.handoff_completed = false;
                aura::serve::mf_mailbox::bump_held_ref_stale_after_steal();
            }
        });
        const auto rejects0 =
            g_mf_mailbox_stats.handoff_reject_total.load(std::memory_order_relaxed);
        // Raw C++ mailbox recv (no orchestrator layer): stale is nullopt.
        auto raw = mb->recv(/*wait=*/false, /*timeout_ms=*/0);
        CHECK(!raw.has_value(), "3642 AC1: raw mailbox recv is nullopt on stale");
        CHECK(g_mf_mailbox_stats.handoff_reject_total.load(std::memory_order_relaxed) >=
                  rejects0 + 1,
              "3642 AC1: handoff_reject_total bumped on raw consume");
        // Queue not jammed: a later plain message still pops.
        MailMessage later3642;
        later3642.payload = "after-stale-3642";
        CHECK(agent_send(h3642, std::move(later3642)) == PushStatus::Ok,
              "3642 AC1: later plain push");
        auto raw2 = mb->recv(/*wait=*/false, /*timeout_ms=*/0);
        CHECK(raw2.has_value() && raw2->payload == "after-stale-3642",
              "3642 AC1: later message pops after stale consume");
        // try_pop: stale consumed as false.
        MailMessage stamped3642b;
        stamped3642b.payload = "stable-ref:12:1";
        stamp_mail_message_handoff_completed(stamped3642b, 12);
        CHECK(agent_send(h3642, std::move(stamped3642b)) == PushStatus::Ok,
              "3642 AC1: second stamped push Ok");
        mb->for_each_pending_held_ref_for_fiber(&dummy3642, [](auto& m) {
            if (m.handoff_completed) {
                m.handoff_completed = false;
                aura::serve::mf_mailbox::bump_held_ref_stale_after_steal();
            }
        });
        MailMessage popped3642;
        CHECK(!mb->try_pop(popped3642), "3642 AC1: try_pop on stale returns false");
        // AC2 plumbing: agent_recv rides the handle flag for the Aura typed
        // fail (the handoff-required hash itself is asserted by source-cite
        // below, same as #3565 AC2).
        MailMessage stamped3642c;
        stamped3642c.payload = "stable-ref:13:1";
        stamp_mail_message_handoff_completed(stamped3642c, 13);
        CHECK(agent_send(h3642, std::move(stamped3642c)) == PushStatus::Ok,
              "3642 AC2: third stamped push Ok");
        mb->for_each_pending_held_ref_for_fiber(&dummy3642, [](auto& m) {
            if (m.handoff_completed) {
                m.handoff_completed = false;
                aura::serve::mf_mailbox::bump_held_ref_stale_after_steal();
            }
        });
        auto helper_got = agent_recv(h3642, /*wait=*/false, /*timeout_ms=*/0);
        CHECK(!helper_got.has_value(), "3642 AC2: agent_recv stale is nullopt (AC1)");
        CHECK(h3642.last_recv_stale_handoff, "3642 AC2: handle flag set for Aura typed fail");
        h3642.last_recv_stale_handoff = false;

        std::println("\n--- #3642 AC6: source-cite + no invent ---");
        const auto mb_src = read_file("src/serve/multi_fiber_mailbox.h");
        const auto spawn_src = read_file("src/orch/agent_spawn.h");
        const auto prim_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto build_src = read_file("build.py");
        CHECK(mb_src.find("Issue #3642") != std::string::npos, "3642 AC6: mailbox cites #3642");
        CHECK(mb_src.find("*stale_handoff = true") != std::string::npos,
              "3642 AC6: recv stale flag out-param");
        CHECK(mb_src.find("(void)maybe_clear_stale_held_ref_on_recv") == std::string::npos,
              "3642 AC6: discarded-return consume sites gone");
        CHECK(spawn_src.find("bool last_recv_stale_handoff = false") != std::string::npos,
              "3642 AC6: handle flag field");
        CHECK(spawn_src.find("Issue #3642") != std::string::npos,
              "3642 AC6: agent_recv cites #3642");
        CHECK(prim_src.find("hp->last_recv_stale_handoff") != std::string::npos,
              "3642 AC6: primitive typed fail rides the flag");
        CHECK(prim_src.find("handoff-required") != std::string::npos,
              "3642 AC6: handoff-required surface unchanged (#3565 AC2)");
        CHECK(build_src.find("check_recv_stale_nullopt_3642") != std::string::npos,
              "3642 AC6: build.py wires linter");
        CHECK(read_file("tests/orch/test_issue_3642.cpp").empty(),
              "3642 AC6: no tests/orch/test_issue file");
        CHECK(read_file("tests/issues/test_issue_3642.cpp").empty(),
              "3642 AC6: no tests/issues file");
        CHECK(read_file("docs/design/3642-recv-stale-nullopt.md").empty(),
              "3642 AC6: no docs/design file");
        apply_dev_audit_defaults();
    }

    // ── Issue #3673: Guard-live Policy A reject → typed deny (not empty=#t)
    {
        std::println("\n--- #3673: agent-recv under-boundary typed deny ---");
        CompilerService cs3673;
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CHECK(cs3673.eval(R"((orch:spawn-agent "3673-a" (lambda () 0) :attach-mailbox #t))")
                  .has_value(),
              "3673 setup: mailbox agent spawned");
        auto& ev3673 = cs3673.evaluator();
        using Evaluator3673 = std::remove_reference_t<decltype(ev3673)>;
        bool guard_ok = true;
        {
            auto guard_r =
                Evaluator3673::MutationBoundaryGuard::try_acquire(ev3673, /*pending=*/1, &guard_ok);
            CHECK(guard_r.has_value(), "3673 setup: Guard try_acquire");
            if (guard_r) {
                auto guard = std::move(*guard_r);
                // AC1: production + Guard-live + wait=#t → typed deny, not empty.
                const auto ok1 =
                    cs3673.eval(R"((hash-ref (orch:agent-recv "3673-a" :wait #t) "ok"))");
                CHECK(ok1 && is_bool(*ok1) && !as_bool(*ok1), "3673 AC1: ok=#f under Guard");
                const auto empty1 =
                    cs3673.eval(R"((hash-ref (orch:agent-recv "3673-a" :wait #t) "empty"))");
                CHECK(empty1 && is_bool(*empty1) && !as_bool(*empty1),
                      "3673 AC1: empty=#f (typed deny, not quiet empty)");
                const auto deny1 =
                    cs3673.eval(R"((hash-ref (orch:agent-recv "3673-a" :wait #t) "deny-detail"))");
                CHECK(deny1 && is_string(*deny1),
                      "3673 AC1: deny-detail present (recv-under-boundary)");
                const auto s3251 =
                    cs3673.eval(R"((hash-ref (orch:agent-recv "3673-a" :wait #t) "schema-3251"))");
                CHECK(s3251 && is_int(*s3251) && as_int(*s3251) == 3251,
                      "3673 AC1: deny interned (#3251)");
                const auto s2347 =
                    cs3673.eval(R"((hash-ref (orch:agent-recv "3673-a" :wait #t) "schema-2347"))");
                CHECK(s2347 && is_int(*s2347) && as_int(*s2347) == 2347,
                      "3673 AC1: schema-2347 row (#2188/#2347 lineage)");
            }
        }
        // #2651: the deny hash interns through push_string_heap. A bare
        // size()+push_back races the spawn body on the shared monotonic
        // resource and SIGSEGVs the next eval's tree-walker lookup
        // (isolated rc=139).
        CHECK(read_file("src/compiler/evaluator_primitives_agent.cpp")
                      .find("push_string_heap(\"recv-under-boundary\")") != std::string::npos,
              "3673: recv deny interns under push_string_heap (#2651)");
        // AC2: no Guard, quiet empty (try semantics — wait #t would park
        // forever on a quiet mailbox; Policy A only short-circuits under
        // Guard) → empty=#t, no deny intern.
        const auto empty2 =
            cs3673.eval(R"((hash-ref (orch:agent-recv "3673-a" :wait #f) "empty"))");
        CHECK(empty2 && is_bool(*empty2) && as_bool(*empty2),
              "3673 AC2: quiet empty stays empty=#t");
        const auto no_deny =
            cs3673.eval(R"((hash-ref (orch:agent-recv "3673-a" :wait #f) "schema-3251"))");
        CHECK(!(no_deny && is_int(*no_deny)), "3673 AC2: no deny-class on quiet empty");
        // AC4: Soft / Off — even under Guard, empty=#t (zero extra intern;
        // the spawn tenant of the deny path is production-gated).
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        {
            auto guard_r2 =
                Evaluator3673::MutationBoundaryGuard::try_acquire(ev3673, /*pending=*/1, &guard_ok);
            if (guard_r2) {
                auto guard2 = std::move(*guard_r2);
                const auto empty4 =
                    cs3673.eval(R"((hash-ref (orch:agent-recv "3673-a" :wait #t) "empty"))");
                CHECK(empty4 && is_bool(*empty4) && as_bool(*empty4),
                      "3673 AC4: Soft + Guard-live stays empty=#t (no typed deny)");
            }
        }
        // Issue #3732: peer recv while this Evaluator holds Guard still
        // delivers a pre-queued message (Policy A is this-fiber only).
        {
            std::println("\n--- #3732 / #3673: peer recv not recv-under-boundary ---");
            aura::compiler::typed_audit::apply_production_audit_defaults();
            auto* hp = cs3673.evaluator().agent_names_->find("3673-a");
            CHECK(hp && hp->ok && hp->mailbox, "3732 peer: handle");
            aura::serve::mf_mailbox::MailMessage queued;
            queued.payload = "peer-3673";
            CHECK(hp->mailbox->push(queued) == aura::serve::mf_mailbox::PushStatus::Ok,
                  "3732 peer: queued");
            bool guard_ok2 = true;
            std::atomic<int> delivered{0};
            {
                auto guard_r3 = Evaluator3673::MutationBoundaryGuard::try_acquire(
                    ev3673, /*pending=*/1, &guard_ok2);
                CHECK(guard_r3.has_value(), "3732 peer: Guard");
                auto guard3 = std::move(*guard_r3);
                std::thread peer([&]() {
                    auto got = aura::orch::agent_recv(*hp, /*wait=*/true, /*timeout_ms=*/500);
                    if (got && got->payload == "peer-3673")
                        delivered.fetch_add(1, std::memory_order_relaxed);
                });
                peer.join();
            }
            CHECK(delivered.load() == 1, "3732 peer: queued message delivered");
            aura::compiler::typed_audit::apply_dev_audit_defaults();
        }
    }

    {
        std::println("\n--- #3940: Guard-live agent_ask is not Policy A timeout ---");
        CompilerService cs3940;
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CHECK(cs3940.eval(R"((orch:spawn-agent "3940-a" (lambda () 0) :attach-mailbox #t))")
                  .has_value(),
              "3940 setup: mailbox agent spawned");
        auto* hp = cs3940.evaluator().agent_names_->find("3940-a");
        CHECK(hp && hp->ok && hp->mailbox, "3940 setup: handle");
        auto& ev3940 = cs3940.evaluator();
        using Ev3940 = std::remove_reference_t<decltype(ev3940)>;
        bool guard_ok = true;
        {
            auto guard_r = Ev3940::MutationBoundaryGuard::try_acquire(ev3940, 1, &guard_ok);
            CHECK(guard_r.has_value(), "3940 setup: Guard try_acquire");
            if (guard_r) {
                auto guard = std::move(*guard_r);
                auto r = aura::orch::agent_ask(*hp, "ping-3940", /*timeout_ms=*/200);
                CHECK(r.status != "timeout",
                      "3940: Guard-live ask is not timeout from Policy A empty");
                CHECK(r.status == "recv-under-boundary", "3940: typed recv-under-boundary");
            }
        }
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        const auto spawn_src = read_file("src/orch/agent_spawn.h");
        CHECK(spawn_src.find("Issue #3940") != std::string::npos, "3940: cite");
        CHECK(spawn_src.find("recv-under-boundary") != std::string::npos,
              "3940: ask recv typed deny");
        CHECK(read_file("tests/orch/test_issue_3940.cpp").empty(), "3940: no test_issue_3940");
    }

    // ── Issue #3733: overflow contract + missing live atomics on the hash ──
    // Fresh Evaluator: the shared `cs` interned thousands of stats keys
    // before this block (plus #3673's second service). Query the facade
    // on a clean string-heap so hash-ref compares the keys just inserted.
    {
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        aura_query_hash_set_force_cap(0);
        CompilerService cs3733;
        auto href3733 = [&](std::string_view key) -> std::int64_t {
            auto r = cs3733.eval(
                std::format("(hash-ref (engine:metrics \"query:orch-module-stats\") \"{}\")", key));
            if (!r || !is_int(*r))
                return -1;
            return as_int(*r);
        };

        std::println("\n--- #3733 AC3: existing keys unchanged ---");
        CHECK(href3733("agents-spawned") >= 0, "3733 AC3: agents-spawned present");
        CHECK(href3733("schema") == 1588, "3733 AC3: schema sentinel 1588");
        CHECK(href3733("schema-2589") == 2589, "3733 AC3: schema-2589 unchanged");
        CHECK(href3733("schema-3529") == 3529, "3733 AC3: schema-3529 unchanged");
        CHECK(href3733("schema-3564") == 3564, "3733 AC3: schema-3564 unchanged");
        CHECK(href3733("hash-overflow") != 1, "3733 AC3: production planned cap does not overflow");
        CHECK(href3733("spawn-tenant-required-total") >= 0,
              "3733 AC3: spawn-tenant-required-total on hash");
        CHECK(href3733("spawn-bp-admit-reject-override-total") >= 0,
              "3733 AC3: spawn-bp-admit-reject-override-total on hash");
        CHECK(href3733("join-reclaimed-deferred-cleanup-total") >= 0,
              "3733 AC3: join-reclaimed-deferred-cleanup-total on hash");
        CHECK(href3733("handoff-join-via-token-total") >= 0,
              "3733 AC3: handoff-join-via-token-total on hash");
        CHECK(href3733("handoff-join-via-token-timeout-total") >= 0,
              "3733 AC3: handoff-join-via-token-timeout-total on hash");
        CHECK(href3733("reclaimed-dtor-under-account-total") >= 0,
              "3733 AC3: reclaimed-dtor-under-account-total on hash");
        CHECK(href3733("workflow-apply-total") >= 0, "3733 AC3: workflow-apply-total on hash");
        CHECK(href3733("schema-3733") == 3733, "3733 AC3: schema-3733");
        CHECK(href3733("orch-module-stats-overflow-wired") == 1,
              "3733 AC3: orch-module-stats-overflow-wired");

        std::println("\n--- #3733 AC1: force-cap never silent-drops ---");
        {
            auto r0 = cs3733.eval("(engine:metrics \"query:orch-module-stats\")");
            CHECK(r0 && is_hash(*r0), "3733 AC1: default hash is non-void");
            aura_query_hash_set_force_cap(4);
            auto r = cs3733.eval("(engine:metrics \"query:orch-module-stats\")");
            CHECK(r && is_hash(*r), "3733 AC1: force-cap hash is non-void (never void/silent)");
            const auto ov = href3733("overflow");
            const auto ho = href3733("hash-overflow");
            CHECK(ov == 1 || ho == 1,
                  "3733 AC1: overflow=1 or hash-overflow=1 (never silent drop)");
            aura_query_hash_set_force_cap(0);
        }
        CHECK(href3733("agents-spawned") >= 0, "3733 AC1: restore cap restores agents-spawned");
        CHECK(href3733("hash-overflow") != 1, "3733 AC1: restore cap clears overflow sentinel");

        std::println("\n--- #3733 AC2: production tenant-required deny on engine:metrics ---");
        {
            using aura::compiler::typed_audit::apply_dev_audit_defaults;
            using aura::compiler::typed_audit::apply_production_audit_defaults;
            using aura::core::sandbox::SandboxMode;
            using aura::core::sandbox::set_mode;
            using aura::orch::AgentSpec;
            using aura::orch::spawn_agent_with_mailbox;
            const char* prev_sb = std::getenv("AURA_SANDBOX");
            const std::string prev_sb_s = prev_sb ? prev_sb : "";
            const auto before = href3733("spawn-tenant-required-total");
            ::setenv("AURA_SANDBOX", "restricted", 1);
            apply_production_audit_defaults();
            set_mode(SandboxMode::Restricted);
            aura::core::resource_quota::set_current_quota_tenant(0);
            aura::core::provenance::set_multi_tenant_env_active(true);
            {
                aura::serve::Scheduler sched(1);
                AgentSpec spec;
                spec.name = "3733-tenant-deny";
                spec.body = [] {};
                auto h = spawn_agent_with_mailbox(sched, std::move(spec));
                CHECK(!h.ok, "3733 AC2: Restricted+MT tenant 0 spawn denied");
                CHECK(h.error == "tenant-required", "3733 AC2: deny string tenant-required");
            }
            aura::core::provenance::set_multi_tenant_env_active(false);
            apply_dev_audit_defaults();
            set_mode(SandboxMode::Off);
            if (!prev_sb_s.empty())
                ::setenv("AURA_SANDBOX", prev_sb_s.c_str(), 1);
            else
                ::unsetenv("AURA_SANDBOX");
            CHECK(href3733("spawn-tenant-required-total") >= before + 1,
                  "3733 AC2: spawn-tenant-required-total ≥ 1 on engine:metrics");
        }

        std::println("\n--- #3733 AC4: Soft same hash, no extra atomics, no invent ---");
        {
            aura::compiler::typed_audit::apply_dev_audit_defaults();
            CHECK(href3733("spawn-tenant-required-total") >= 0,
                  "3733 AC4: Soft still exposes spawn-tenant-required-total");
            CHECK(href3733("workflow-apply-total") >= 0,
                  "3733 AC4: Soft still exposes workflow-apply-total");
            CHECK(href3733("agents-spawned") >= 0, "3733 AC4: Soft agents-spawned present");
            const auto agent = read_file("src/compiler/evaluator_primitives_agent.cpp");
            CHECK(agent.find("kOrchModuleStatsPlannedKeys") != std::string::npos,
                  "3733 AC4: planned-keys constant");
            CHECK(agent.find("insert_kv_checked") != std::string::npos,
                  "3733 AC4: insert_kv_checked (not silent probe)");
            CHECK(agent.find("query_hash_finish") != std::string::npos,
                  "3733 AC4: query_hash_finish");
            CHECK(agent.find("query_hash_capacity_for(kOrchModuleStatsPlannedKeys)") !=
                      std::string::npos,
                  "3733 AC4: capacity from planned keys");
            const auto key_pos = agent.find("insert_kv(\"spawn-tenant-required-total\"");
            CHECK(key_pos != std::string::npos, "3733 AC4: spawn-tenant-required-total insert");
            const auto finish_pos = agent.find("return query_hash_finish(ht, ev.string_heap_");
            CHECK(finish_pos != std::string::npos && key_pos < finish_pos,
                  "3733 AC4: new keys append before query_hash_finish");
            CHECK(agent.find("class AgentRegistry") == std::string::npos &&
                      agent.find("struct AgentRegistry") == std::string::npos,
                  "3733 AC4: no AgentRegistry");
            std::ifstream invent("tests/orch/test_issue_3733.cpp");
            if (!invent.good())
                invent.open("../tests/orch/test_issue_3733.cpp");
            CHECK(!invent.good(), "3733 AC4: no test_issue_3733.cpp");
            CHECK(read_file("docs/design/3733-orch-module-stats-overflow.md").empty(),
                  "3733 AC4: no docs/design/3733-*");
            const auto readme = read_file("src/orch/README.md");
            CHECK(readme.find("spawn-tenant-required-total") != std::string::npos,
                  "3733 AC4: README lists spawn-tenant-required-total");
            CHECK(readme.find("insert_kv_checked") != std::string::npos,
                  "3733 AC4: README documents overflow contract");
        }
    }

    {
        std::println("\n--- #4001: Guard-live C++ recv is typed RecvResult, not nullopt wait ---");
        using aura::orch::agent_recv;
        using aura::orch::agent_recv_result;
        using aura::orch::agent_recv_safe;
        using aura::orch::kRecvTypedStatusIssue;
        CHECK(kRecvTypedStatusIssue == 4001, "4001: issue stamp");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        CompilerService cs4001;
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CHECK(cs4001.eval(R"((orch:spawn-agent "4001-a" (lambda () 0) :attach-mailbox #t))")
                  .has_value(),
              "4001 setup: mailbox agent spawned");
        auto* hp = cs4001.evaluator().agent_names_->find("4001-a");
        CHECK(hp && hp->ok && hp->mailbox, "4001 setup: handle");
        auto& ev4001 = cs4001.evaluator();
        using Ev4001 = std::remove_reference_t<decltype(ev4001)>;
        bool guard_ok = true;
        {
            auto guard_r = Ev4001::MutationBoundaryGuard::try_acquire(ev4001, 1, &guard_ok);
            CHECK(guard_r.has_value(), "4001 setup: Guard try_acquire");
            if (guard_r) {
                auto guard = std::move(*guard_r);
                auto r = agent_recv_result(*hp, /*wait=*/true, /*timeout_ms=*/1000);
                CHECK(!r.ok, "4001 AC1: RecvResult not ok");
                CHECK(std::string_view(r.status) == "recv-under-boundary",
                      "4001 AC1: status=recv-under-boundary (not empty)");
                CHECK(!r.message, "4001 AC1: no payload");
                auto rs = agent_recv_safe(*hp, /*wait=*/true, /*timeout_ms=*/1000);
                CHECK(std::string_view(rs.status) == "recv-under-boundary",
                      "4001 AC1: agent_recv_safe same status");
                auto raw = agent_recv(*hp, /*wait=*/true, /*timeout_ms=*/1000);
                CHECK(!raw.has_value(), "4001 AC1: raw agent_recv stays nullopt");
                CHECK(hp->last_recv_boundary_reject, "4001 AC1: flag still rides the handle");
                const auto aura_st =
                    cs4001.eval(R"((hash-ref (orch:agent-recv "4001-a" :wait #t) "status"))");
                CHECK(aura_st && is_string(*aura_st), "4001 AC2: Aura typed deny unchanged");
            }
        }
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        {
            auto empty = agent_recv_result(*hp, /*wait=*/false, /*timeout_ms=*/0);
            CHECK(!empty.ok, "4001 AC3: Soft empty not ok");
            CHECK(std::string_view(empty.status) == "empty",
                  "4001 AC3: Soft quiet empty status=empty");
            bool guard_ok2 = true;
            auto guard_r2 = Ev4001::MutationBoundaryGuard::try_acquire(ev4001, 1, &guard_ok2);
            if (guard_r2) {
                auto guard2 = std::move(*guard_r2);
                auto soft_g = agent_recv_result(*hp, /*wait=*/true, /*timeout_ms=*/1000);
                CHECK(std::string_view(soft_g.status) == "empty",
                      "4001 AC3: Soft Guard-live stays empty (no typed deny)");
            }
        }
        const auto spawn_src = read_file("src/orch/agent_spawn.h");
        const auto agent_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto readme = read_file("src/orch/README.md");
        CHECK(spawn_src.find("kRecvTypedStatusIssue = 4001") != std::string::npos,
              "4001 AC5: stamp");
        CHECK(spawn_src.find("struct RecvResult") != std::string::npos, "4001 AC5: RecvResult");
        CHECK(spawn_src.find("agent_recv_result") != std::string::npos, "4001 AC5: result helper");
        CHECK(spawn_src.find("agent_recv_safe") != std::string::npos, "4001 AC5: safe alias");
        CHECK(agent_src.find("agent_recv_result") != std::string::npos,
              "4001 AC5: Aura uses RecvResult SSOT");
        CHECK(agent_src.find("query:4001") == std::string::npos, "4001 AC5: no new query key");
        CHECK(spawn_src.find("class AgentRegistry") == std::string::npos,
              "4001 AC5: no AgentRegistry");
        CHECK(readme.find("#4001") != std::string::npos, "4001 AC5: README cites C++ RecvResult");
        CHECK(read_file("tests/orch/test_issue_4001.cpp").empty(),
              "4001 AC5: no test_issue_4001.cpp");
        CHECK(read_file("docs/design/4001-recv-typed-status.md").empty(),
              "4001 AC5: no docs/design/4001-*");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
    }

    std::println("\n=== #2589+#2636+2884+#3013+#3212+#3251+#3336+#3565+#3642+#3733+#4001: {}/{} "
                 "checks passed ===",
                 g_passed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_orch_obs_facade();
}
#endif
