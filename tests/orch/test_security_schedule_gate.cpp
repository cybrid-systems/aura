// tests/orch/test_security_schedule_gate.cpp
// @category: integration
// @reason: Issue #2590 — pure `decide_security_schedule()` synthesizes
//          commit_readiness (#2553) + capability deny rate (#2534 trail) +
//          mid-fallback SLO breach + posture wal_off under Restricted
//          (#2076). Production default denies new mutate when gate flips
//          false; soft / sandbox=off is observe-only. Counters always
//          bump. Test verifies pure idempotency, production/soft matrix,
//          counter bumps, query surface, and README source-cite.
//
//   AC1: Pure decide_security_schedule — same input → same output,
//        no side effects (verified by idempotency + post-call counter
//        snapshot before/after pure call = 0).
//   AC2: Production + commit_readiness hard reject → deny with
//        reason=commit_not_ready + counter deny_commit_not_ready_total++
//        + last_would_allow=0.
//   AC3: Soft mode + same inputs → allow (observe-only). Counters
//        still bump (allow_total++) but no deny.
//   AC4: #2543 AOT throttle regression guard — we never call
//        decide_hot_update_throttle / sample_aot_hot_update_health_*,
//        and g_orch_hot_update_health_* counters are unchanged by
//        evaluate_security_schedule (verified by pre/post snapshot).
//   AC5: query:security-schedule-gate exposes would-allow-new-mutate,
//        force-reason-code, checks/deny/allow totals, per-reason deny
//        totals, security-schedule-gate-wired, schema-2590, issue-2590.
//
// Source-cite (issue #2590):
//   - src/orch/security_schedule_gate.h: decide_security_schedule
//     (pure), evaluate_security_schedule (pure + counters),
//     g_orch_security_schedule_counters (atomics),
//     reset_orch_security_schedule_counters_for_test (test reset).
//   - src/compiler/evaluator_primitives_security.cpp:
//     query:security-schedule-gate primitive (FlatHashTable of
//     would-allow-new-mutate / force-reason-code / counters).
//   - src/orch/README.md: "Security schedule gate (Issue #2590)"
//     section with decision table + call sites + source-cite.
//   - tests/orch/test_security_schedule_gate.cpp (this file).
//
// No docs/design/ per #1655 / #1485.

#include "test_harness.hpp"

#include "orch/security_schedule_gate.h"
#include "compiler/aot_hot_update_health.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/wal_append_fail_slo.h"
#include "orch/agent_spawn.h"
#include "orch/sched_runner_test_helper.h"
#include "serve/scheduler.h"

extern "C" void aura_engine_metrics_reset_hash_overflow_for_test(void);
extern "C" void aura_query_hash_set_force_cap(std::uint64_t);
extern "C" void aura_query_hash_reset_overflow_for_test(void);

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
using aura::compiler::g_orch_hot_update_health_checks_total;
using aura::compiler::g_orch_hot_update_health_last_force_reason;
using aura::compiler::g_orch_hot_update_health_throttle_total;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::orch::decide_security_schedule;
using aura::orch::evaluate_security_schedule;
using aura::orch::g_orch_security_schedule_counters;
using aura::orch::reset_orch_security_schedule_counters_for_test;
using aura::orch::SecurityScheduleForceReason;
using aura::orch::SecurityScheduleInput;
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
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:security-schedule-gate\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static SecurityScheduleInput base_input() {
    SecurityScheduleInput in;
    in.commit_readiness_would_allow = true;
    in.commit_readiness_hard_reject = false;
    in.capability_deny_storm = false;
    in.mid_fallback_slo_breach = false;
    in.posture_wal_off_restricted = false;
    in.production_mode = false;
    in.soft_mode = false;
    return in;
}

} // namespace

int run_test_security_schedule_gate() {
    std::println("=== Issue #2590: security schedule gate ===");

    // ── README source-cite (AC5) ──
    {
        std::println("\n--- #2590 README source-cite ---");
        const auto readme_src = read_file("src/orch/README.md");
        CHECK(readme_src.find("Security schedule gate") != std::string::npos,
              "AC5: README has 'Security schedule gate' section");
        CHECK(readme_src.find("Issue #2590") != std::string::npos, "AC5: README cites Issue #2590");
        CHECK(readme_src.find("query:security-schedule-gate") != std::string::npos,
              "AC5: README cites query:security-schedule-gate primitive");
        CHECK(readme_src.find("decide_security_schedule") != std::string::npos,
              "AC5: README documents decide_security_schedule");
        CHECK(readme_src.find("evaluate_security_schedule") != std::string::npos,
              "AC5: README documents evaluate_security_schedule");
        CHECK(readme_src.find("commit-not-ready") != std::string::npos,
              "AC5: README decision table includes commit-not-ready");
        CHECK(readme_src.find("deny-storm") != std::string::npos,
              "AC5: README decision table includes deny-storm");
        CHECK(readme_src.find("mid-fallback-slo") != std::string::npos,
              "AC5: README decision table includes mid-fallback-slo");
        CHECK(readme_src.find("posture-degraded") != std::string::npos,
              "AC5: README decision table includes posture-degraded");
    }

    CompilerService cs;

    // ── AC1: Pure decide_security_schedule — same input → same output ──
    {
        std::println("\n--- #2590 AC1: pure idempotency ---");
        reset_orch_security_schedule_counters_for_test();
        auto in = base_input();
        in.production_mode = true;
        in.commit_readiness_would_allow = false;
        in.commit_readiness_hard_reject = true;
        const auto d1 = decide_security_schedule(in);
        const auto d2 = decide_security_schedule(in);
        CHECK(d1.would_allow_new_mutate == d2.would_allow_new_mutate,
              "AC1: pure decision idempotent (would_allow_new_mutate)");
        CHECK(d1.force_reason == d2.force_reason, "AC1: pure decision idempotent (force_reason)");
        CHECK(d1.force_reason == SecurityScheduleForceReason::commit_not_ready,
              "AC1: commit-not-ready reason when hard reject in production");
        CHECK(!d1.would_allow_new_mutate, "AC1: production + commit_not_ready hard reject → deny");
        // Pure call must not bump counters (#2590 AC1).
        CHECK(g_orch_security_schedule_counters.checks_total.load(std::memory_order_relaxed) == 0,
              "AC1: pure call does not bump checks_total");
    }

    // ── AC2: Production + commit_readiness hard reject → deny + metric ──
    {
        std::println("\n--- #2590 AC2: production hard reject → deny ---");
        reset_orch_security_schedule_counters_for_test();
        auto in = base_input();
        in.production_mode = true;
        in.commit_readiness_would_allow = false;
        in.commit_readiness_hard_reject = true;
        const auto d = evaluate_security_schedule(in);
        CHECK(!d.would_allow_new_mutate,
              "AC2: production + hard reject → would_allow_new_mutate=false");
        CHECK(d.force_reason == SecurityScheduleForceReason::commit_not_ready,
              "AC2: force_reason = commit_not_ready");
        CHECK(g_orch_security_schedule_counters.checks_total.load(std::memory_order_relaxed) == 1,
              "AC2: checks_total bumped");
        CHECK(g_orch_security_schedule_counters.deny_total.load(std::memory_order_relaxed) == 1,
              "AC2: deny_total bumped");
        CHECK(g_orch_security_schedule_counters.allow_total.load(std::memory_order_relaxed) == 0,
              "AC2: allow_total NOT bumped");
        CHECK(g_orch_security_schedule_counters.deny_commit_not_ready_total.load(
                  std::memory_order_relaxed) == 1,
              "AC2: deny_commit_not_ready_total bumped");
        CHECK(g_orch_security_schedule_counters.last_would_allow.load(std::memory_order_relaxed) ==
                  0,
              "AC2: last_would_allow = 0");
        CHECK(g_orch_security_schedule_counters.last_force_reason_code.load(
                  std::memory_order_relaxed) ==
                  static_cast<std::int64_t>(SecurityScheduleForceReason::commit_not_ready),
              "AC2: last_force_reason_code = commit_not_ready");
    }

    // ── AC3: Soft / sandbox=off → observe only ──
    {
        std::println("\n--- #2590 AC3: soft mode → observe only ---");
        reset_orch_security_schedule_counters_for_test();
        auto in = base_input();
        in.production_mode = true;
        in.soft_mode = true; // soft overrides production
        in.commit_readiness_would_allow = false;
        in.commit_readiness_hard_reject = true;
        const auto d = evaluate_security_schedule(in);
        CHECK(d.would_allow_new_mutate, "AC3: soft + hard reject → allow (observe-only)");
        CHECK(d.force_reason == SecurityScheduleForceReason::ok, "AC3: soft → force_reason = ok");
        CHECK(g_orch_security_schedule_counters.checks_total.load(std::memory_order_relaxed) == 1,
              "AC3: soft still bumps checks_total");
        CHECK(g_orch_security_schedule_counters.allow_total.load(std::memory_order_relaxed) == 1,
              "AC3: soft bumps allow_total");
        CHECK(g_orch_security_schedule_counters.deny_total.load(std::memory_order_relaxed) == 0,
              "AC3: soft does NOT bump deny_total");
    }

    // ── Additional decision matrix: deny_storm / mid_fallback_slo / posture_degraded ──
    {
        std::println(
            "\n--- #2590 decision matrix: deny_storm / mid_fallback_slo / posture_degraded ---");
        reset_orch_security_schedule_counters_for_test();

        auto in_deny_storm = base_input();
        in_deny_storm.production_mode = true;
        in_deny_storm.capability_deny_storm = true;
        const auto d_ds = evaluate_security_schedule(in_deny_storm);
        CHECK(!d_ds.would_allow_new_mutate, "matrix: deny_storm production → deny");
        CHECK(d_ds.force_reason == SecurityScheduleForceReason::deny_storm,
              "matrix: deny_storm reason");
        CHECK(g_orch_security_schedule_counters.deny_deny_storm_total.load(
                  std::memory_order_relaxed) == 1,
              "matrix: deny_deny_storm_total bumped");

        auto in_mid = base_input();
        in_mid.production_mode = true;
        in_mid.mid_fallback_slo_breach = true;
        const auto d_mid = evaluate_security_schedule(in_mid);
        CHECK(!d_mid.would_allow_new_mutate, "matrix: mid_fallback_slo → deny");
        CHECK(d_mid.force_reason == SecurityScheduleForceReason::mid_fallback_slo,
              "matrix: mid_fallback_slo reason");
        CHECK(g_orch_security_schedule_counters.deny_mid_fallback_slo_total.load(
                  std::memory_order_relaxed) == 1,
              "matrix: deny_mid_fallback_slo_total bumped");

        auto in_posture = base_input();
        in_posture.production_mode = true;
        in_posture.posture_wal_off_restricted = true;
        const auto d_p = evaluate_security_schedule(in_posture);
        CHECK(!d_p.would_allow_new_mutate, "matrix: posture_degraded → deny");
        CHECK(d_p.force_reason == SecurityScheduleForceReason::posture_degraded,
              "matrix: posture_degraded reason");
        CHECK(g_orch_security_schedule_counters.deny_posture_degraded_total.load(
                  std::memory_order_relaxed) == 1,
              "matrix: deny_posture_degraded_total bumped");

        // Non-production with all signals on → still allow (no enforcement).
        auto in_off = base_input();
        in_off.production_mode = false;
        in_off.commit_readiness_would_allow = false;
        in_off.commit_readiness_hard_reject = true;
        in_off.capability_deny_storm = true;
        in_off.mid_fallback_slo_breach = true;
        in_off.posture_wal_off_restricted = true;
        const auto d_off = evaluate_security_schedule(in_off);
        CHECK(d_off.would_allow_new_mutate,
              "matrix: non-production + all signals → allow (no enforcement)");
        CHECK(d_off.force_reason == SecurityScheduleForceReason::ok, "matrix: non-production → ok");
    }

    // ── AC4: #2543 AOT throttle regression guard ──
    {
        std::println("\n--- #2590 AC4: #2543 AOT throttle regression guard ---");
        // Snapshot the #2543 counters, exercise evaluate_security_schedule
        // aggressively, then verify they're unchanged.
        const auto pre_checks =
            g_orch_hot_update_health_checks_total.load(std::memory_order_relaxed);
        const auto pre_throttle =
            g_orch_hot_update_health_throttle_total.load(std::memory_order_relaxed);
        const auto pre_reason =
            g_orch_hot_update_health_last_force_reason.load(std::memory_order_relaxed);

        reset_orch_security_schedule_counters_for_test();
        for (int i = 0; i < 5; ++i) {
            auto in = base_input();
            in.production_mode = true;
            in.commit_readiness_would_allow = false;
            in.commit_readiness_hard_reject = true;
            (void)evaluate_security_schedule(in);
        }

        CHECK(g_orch_hot_update_health_checks_total.load(std::memory_order_relaxed) == pre_checks,
              "AC4: #2543 checks_total unchanged after evaluate_security_schedule");
        CHECK(g_orch_hot_update_health_throttle_total.load(std::memory_order_relaxed) ==
                  pre_throttle,
              "AC4: #2543 throttle_total unchanged after evaluate_security_schedule");
        CHECK(g_orch_hot_update_health_last_force_reason.load(std::memory_order_relaxed) ==
                  pre_reason,
              "AC4: #2543 last_force_reason unchanged after evaluate_security_schedule");
    }

    // ── AC5: query:security-schedule-gate surface ──
    {
        std::println("\n--- #2590 AC5: query:security-schedule-gate ---");
        reset_orch_security_schedule_counters_for_test();
        // Drive two deny events to make counters visible.
        for (int i = 0; i < 2; ++i) {
            auto in = base_input();
            in.production_mode = true;
            in.commit_readiness_would_allow = false;
            in.commit_readiness_hard_reject = true;
            (void)evaluate_security_schedule(in);
        }
        // And one allow event.
        {
            auto in = base_input();
            (void)evaluate_security_schedule(in);
        }
        CHECK(href(cs, "checks-total") == 3, "AC5: query checks-total = 3");
        CHECK(href(cs, "deny-total") == 2, "AC5: query deny-total = 2");
        CHECK(href(cs, "allow-total") == 1, "AC5: query allow-total = 1");
        CHECK(href(cs, "deny-commit-not-ready-total") == 2,
              "AC5: query deny-commit-not-ready-total = 2");
        CHECK(href(cs, "would-allow-new-mutate") == 1,
              "AC5: query would-allow-new-mutate reflects last (allow) decision");
        CHECK(href(cs, "force-reason-code") == 0,
              "AC5: query force-reason-code = ok after last allow call");
        CHECK(href(cs, "security-schedule-gate-wired") == 1,
              "AC5: security-schedule-gate-wired sentinel = 1");
        CHECK(href(cs, "schema-2590") == 2590, "AC5: schema-2590 present");
        CHECK(href(cs, "issue-2590") == 2590, "AC5: issue-2590 present");
    }

    // ─── Issue #2660: production admit wiring (live signals) ───
    //   AC1: Production + commit_readiness_hard_reject → reject.
    //   AC2: Production + capability deny storm → reject with deny_storm.
    //   AC3: All-clear inputs → allow (no extra alloc on happy path).
    //   AC4: #2587 mailbox starvation gate still fires independently.
    //   AC5: query:security-schedule-gate last decision mirrors deny.
    //   AC6: source-cite + coverage linter (no docs/design per #1655).

    // AC1: commit_readiness_live_signals + admit_security_schedule.
    {
        std::println("\n--- #2660 AC1: commit_not_ready → reject ---");
        reset_orch_security_schedule_counters_for_test();
        const auto signals = aura::orch::commit_readiness_live_signals();
        std::println("  commit_readiness_live: would_allow={} hard_reject={}", signals.first,
                     signals.second);
        // Production + commit_readiness_hard_reject → reject.
        aura::orch::SecurityScheduleInput in;
        in.production_mode = true;
        in.soft_mode = false;
        in.commit_readiness_would_allow = signals.first;
        in.commit_readiness_hard_reject = signals.second;
        in.capability_deny_storm = false;
        in.mid_fallback_slo_breach = false;
        in.posture_wal_off_restricted = false;
        const auto allow_a = aura::orch::admit_security_schedule(in);
        // Force production + hard_reject + !would_allow → reject.
        in.commit_readiness_would_allow = false;
        in.commit_readiness_hard_reject = true;
        const auto reject_a = aura::orch::admit_security_schedule(in);
        std::println("  admit: allow={} reject={}", !allow_a.has_value(), reject_a.has_value());
        CHECK(reject_a.has_value(), "AC1: production + hard_reject → reject");
        const auto reason = reject_a.value_or("");
        CHECK(reason.find("commit-not-ready") != std::string::npos ||
                  reason.find("security-schedule") != std::string::npos,
              "AC1: reject reason carries force_reason name");
        // Soft / sandbox=off: same input → allow (observe-only).
        in.production_mode = false;
        in.soft_mode = true;
        const auto soft_a = aura::orch::admit_security_schedule(in);
        CHECK(!soft_a.has_value(), "AC1: soft mode + hard_reject → allow (observe-only)");
    }

    // AC2: capability deny storm → reject with force_reason=deny_storm.
    {
        std::println("\n--- #2660 AC2: deny_storm threshold → reject ---");
        reset_orch_security_schedule_counters_for_test();
        // Lower the threshold so a single denial is enough to trip the storm.
        aura::orch::g_capability_deny_storm_threshold().store(1, std::memory_order_relaxed);
        // Bump the process counter (set + reset helper for tests).
        auto& met = aura::core::capability::g_capability_effect_metrics();
        const auto before = met.capability_effect_denied_total.load(std::memory_order_relaxed);
        met.capability_effect_denied_total.store(before + 5, std::memory_order_relaxed);
        // Production + deny_storm → reject.
        aura::orch::SecurityScheduleInput in;
        in.production_mode = true;
        in.soft_mode = false;
        in.commit_readiness_would_allow = true;
        in.commit_readiness_hard_reject = false;
        in.capability_deny_storm = aura::orch::capability_deny_storm_live();
        in.mid_fallback_slo_breach = false;
        in.posture_wal_off_restricted = false;
        CHECK(in.capability_deny_storm, "AC2: capability_deny_storm_live = true under threshold");
        const auto rej = aura::orch::admit_security_schedule(in);
        CHECK(rej.has_value(), "AC2: production + deny_storm → reject");
        const auto reason = rej.value_or("");
        CHECK(reason.find("deny-storm") != std::string::npos, "AC2: reject reason = deny-storm");
        // Soft mode → allow.
        in.production_mode = false;
        in.soft_mode = true;
        CHECK(!aura::orch::admit_security_schedule(in).has_value(),
              "AC2: soft + deny_storm → allow (observe-only)");
        // Restore threshold.
        aura::orch::g_capability_deny_storm_threshold().store(64, std::memory_order_relaxed);
        met.capability_effect_denied_total.store(before, std::memory_order_relaxed);
    }

    // AC3: all-clear inputs → allow (no extra alloc on happy path).
    {
        std::println("\n--- #2660 AC3: all-clear → allow ---");
        reset_orch_security_schedule_counters_for_test();
        aura::orch::SecurityScheduleInput in;
        in.production_mode = true;
        in.soft_mode = false;
        in.commit_readiness_would_allow = true;
        in.commit_readiness_hard_reject = false;
        in.capability_deny_storm = false;
        in.mid_fallback_slo_breach = false;
        in.posture_wal_off_restricted = false;
        const auto allow = aura::orch::admit_security_schedule(in);
        CHECK(!allow.has_value(), "AC3: all-clear inputs → allow (nullopt)");
        // decision matches: would_allow_new_mutate = true.
        const auto d = aura::orch::decide_security_schedule(in);
        CHECK(d.would_allow_new_mutate, "AC3: decide_security_schedule all-clear → allow");
    }

    // AC4: #2587 mailbox starvation gate still fires independently.
    // We verify the function symbols exist + the integration site
    // (evaluator_mutation_boundary.cpp) still calls both gates.
    // Symbol is aura_orch_mailbox_starvation_throttled (header-inline
    // probe in multi_fiber_mailbox.h — not the short aura_mailbox_* alias).
    {
        std::println("\n--- #2660 AC4: #2587 mailbox starvation gate still independent ---");
        const auto mbc = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(mbc.find("aura_orch_mailbox_starvation_throttled") != std::string::npos,
              "AC4: #2587 mailbox starvation gate still wired in try_acquire");
        CHECK(mbc.find("make_security_schedule_input_live") != std::string::npos ||
                  mbc.find("evaluate_security_schedule") != std::string::npos ||
                  mbc.find("admit_security_schedule") != std::string::npos,
              "AC4: #2590/#2660 security-schedule gate still wired");
        // Both gates observable in the same site — order documented in
        // evaluator_mutation_boundary.cpp: #2587 first, then #2590/#2660/#2947.
        const auto mb_pos = mbc.find("aura_orch_mailbox_starvation_throttled");
        const auto ss_pos = mbc.find("make_security_schedule_input_live");
        const auto ss_pos2 = mbc.find("admit_security_schedule");
        const auto ss_eff = (ss_pos != std::string::npos) ? ss_pos : ss_pos2;
        CHECK(mb_pos != std::string::npos && ss_eff != std::string::npos && mb_pos < ss_eff,
              "AC4: #2587 mailbox starvation gate fires before #2590/#2660 security-schedule");
    }

    // AC5: query:security-schedule-gate last decision mirrors the deny.
    {
        std::println("\n--- #2660 AC5: query mirrors deny ---");
        reset_orch_security_schedule_counters_for_test();
        // Drive a deny via post_not_ready.
        aura::orch::SecurityScheduleInput in;
        in.production_mode = true;
        in.soft_mode = false;
        in.commit_readiness_would_allow = false;
        in.commit_readiness_hard_reject = true;
        in.capability_deny_storm = false;
        in.mid_fallback_slo_breach = false;
        in.posture_wal_off_restricted = false;
        (void)aura::orch::evaluate_security_schedule(in);
        // The query surface reflects the last decision.
        CHECK(href(cs, "would-allow-new-mutate") == 0,
              "AC5: query would-allow-new-mutate = 0 after deny");
        CHECK(href(cs, "force-reason-code") ==
                  static_cast<std::int64_t>(
                      aura::orch::SecurityScheduleForceReason::commit_not_ready),
              "AC5: query force-reason-code = commit-not-ready");
    }

    // AC6: source-cite + coverage linter (no docs/design per #1655).
    {
        std::println("\n--- #2660 AC6: source-cite + coverage ---");
        const auto gate_h = read_file("src/orch/security_schedule_gate.h");
        CHECK(gate_h.find("make_security_schedule_input_live") != std::string::npos,
              "AC6: make_security_schedule_input_live helper present");
        CHECK(gate_h.find("admit_security_schedule") != std::string::npos,
              "AC6: admit_security_schedule helper present");
        CHECK(gate_h.find("commit_readiness_live_signals") != std::string::npos,
              "AC6: commit_readiness_live_signals helper present");
        CHECK(gate_h.find("capability_deny_storm_live") != std::string::npos,
              "AC6: capability_deny_storm_live helper present");
        CHECK(gate_h.find("mid_fallback_slo_breach_live") != std::string::npos,
              "AC6: mid_fallback_slo_breach_live helper present");
        CHECK(gate_h.find("posture_wal_off_restricted_live") != std::string::npos,
              "AC6: posture_wal_off_restricted_live helper present");
        const auto mbc = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(mbc.find("make_security_schedule_input_live") != std::string::npos,
              "AC6: try_acquire / try_acquire_for_region use live helper");
        const auto agent = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(agent.find("make_security_schedule_input_live") != std::string::npos,
              "AC6: parallel-intend uses live helper");
        // Coverage manifest + linter.
        const auto ck = read_file("scripts/coverage/checks/check_2660.py");
        CHECK(!ck.empty(), "AC6: coverage linter check_2660.py present");
        const auto mf = read_file("scripts/coverage/manifests/2660.json");
        CHECK(!mf.empty(), "AC6: coverage manifest 2660.json present");
        CHECK(read_file("docs/design/2660-security-schedule-admit.md").empty(),
              "AC6: no docs/design/ — design rationale in commit + close comment");
    }

    // ─── Issue #2947: mailbox under-boundary wait p99 → gate deny ───
    // AC1: production + high p99 or throttle → deny mailbox_hold_slo
    // AC2: Soft → allow for this reason alone
    // AC3: zero samples / zero throttle → allow (quiet)
    // AC4: commit_not_ready still wins over mailbox signal
    // AC5: schema-2947 + deny-mailbox-hold-slo-total query keys
    // AC6: source-cite + linter; no docs/design per #1655
    {
        std::println("\n--- #2947 AC1–AC6: mailbox hold SLO security schedule ---");
        CHECK(aura::orch::kSecurityScheduleMailboxHoldSloIssue == 2947, "2947: issue stamp");

        // AC1: production + synthetic high p99 → deny
        {
            reset_orch_security_schedule_counters_for_test();
            auto in = base_input();
            in.production_mode = true;
            in.soft_mode = false;
            in.mailbox_wait_p99_us = 250'000; // 250 ms
            in.mailbox_wait_slo_us = 100'000; // 100 ms default
            CHECK(aura::orch::mailbox_hold_slo_signal(in),
                  "2947 AC1: high p99 trips mailbox_hold_slo_signal");
            const auto d = evaluate_security_schedule(in);
            CHECK(!d.would_allow_new_mutate, "2947 AC1: production + high p99 → deny");
            CHECK(d.force_reason == SecurityScheduleForceReason::mailbox_hold_slo,
                  "2947 AC1: force_reason = mailbox_hold_slo");
            CHECK(g_orch_security_schedule_counters.deny_mailbox_hold_slo_total.load(
                      std::memory_order_relaxed) == 1,
                  "2947 AC1: deny_mailbox_hold_slo_total++");
            const auto rej = aura::orch::admit_security_schedule(in);
            CHECK(rej.has_value(), "2947 AC1: admit rejects under production");
            CHECK(rej.value_or("").find("mailbox-hold-slo") != std::string::npos,
                  "2947 AC1: admit reason = mailbox-hold-slo");
        }

        // AC1b: throttle flag alone → deny
        {
            reset_orch_security_schedule_counters_for_test();
            auto in = base_input();
            in.production_mode = true;
            in.mailbox_starvation_throttled = true;
            in.mailbox_wait_slo_us = 100'000;
            in.mailbox_wait_p99_us = 0; // no p99 samples
            const auto d = evaluate_security_schedule(in);
            CHECK(!d.would_allow_new_mutate, "2947 AC1: throttle alone → deny");
            CHECK(d.force_reason == SecurityScheduleForceReason::mailbox_hold_slo,
                  "2947 AC1: throttle force_reason = mailbox_hold_slo");
        }

        // AC2: Soft + high p99 → allow (observe-only)
        {
            reset_orch_security_schedule_counters_for_test();
            auto in = base_input();
            in.production_mode = false;
            in.soft_mode = true;
            in.mailbox_wait_p99_us = 500'000;
            in.mailbox_wait_slo_us = 100'000;
            in.mailbox_starvation_throttled = true;
            const auto d = evaluate_security_schedule(in);
            CHECK(d.would_allow_new_mutate,
                  "2947 AC2: Soft → would_allow stays true for mailbox reason alone");
            CHECK(d.force_reason == SecurityScheduleForceReason::ok,
                  "2947 AC2: Soft force_reason = ok");
            CHECK(g_orch_security_schedule_counters.allow_total.load(std::memory_order_relaxed) ==
                      1,
                  "2947 AC2: Soft bumps allow_total");
            CHECK(g_orch_security_schedule_counters.deny_mailbox_hold_slo_total.load(
                      std::memory_order_relaxed) == 0,
                  "2947 AC2: Soft does not bump deny_mailbox_hold_slo_total");
            CHECK(!aura::orch::admit_security_schedule(in).has_value(),
                  "2947 AC2: Soft admit never rejects on mailbox alone");
        }

        // AC3: zero samples + no throttle → allow under production
        {
            reset_orch_security_schedule_counters_for_test();
            auto in = base_input();
            in.production_mode = true;
            in.mailbox_wait_p99_us = 0;
            in.mailbox_wait_slo_us = 100'000;
            in.mailbox_starvation_throttled = false;
            CHECK(!aura::orch::mailbox_hold_slo_signal(in), "2947 AC3: quiet path signal false");
            const auto d = decide_security_schedule(in);
            CHECK(d.would_allow_new_mutate, "2947 AC3: zero samples → allow");
            CHECK(d.force_reason == SecurityScheduleForceReason::ok, "2947 AC3: force_reason ok");
            // SLO=0 disables p99 arm even with high p99
            in.mailbox_wait_p99_us = 999'999;
            in.mailbox_wait_slo_us = 0;
            CHECK(!aura::orch::mailbox_hold_slo_signal(in), "2947 AC3: slo=0 disables latency arm");
        }

        // AC4: commit_not_ready wins over mailbox (priority)
        {
            reset_orch_security_schedule_counters_for_test();
            auto in = base_input();
            in.production_mode = true;
            in.commit_readiness_would_allow = false;
            in.commit_readiness_hard_reject = true;
            in.mailbox_wait_p99_us = 500'000;
            in.mailbox_wait_slo_us = 100'000;
            in.mailbox_starvation_throttled = true;
            const auto d = decide_security_schedule(in);
            CHECK(!d.would_allow_new_mutate, "2947 AC4: still deny");
            CHECK(d.force_reason == SecurityScheduleForceReason::commit_not_ready,
                  "2947 AC4: mailbox does not mask commit_not_ready");
            CHECK(d.force_reason != SecurityScheduleForceReason::mailbox_hold_slo,
                  "2947 AC4: force_reason is not mailbox_hold_slo when commit hot");
        }

        // AC5 / AC6: query + source-cite
        {
            reset_orch_security_schedule_counters_for_test();
            auto in = base_input();
            in.production_mode = true;
            in.mailbox_wait_p99_us = 200'000;
            in.mailbox_wait_slo_us = 100'000;
            (void)evaluate_security_schedule(in);
            CHECK(href(cs, "deny-mailbox-hold-slo-total") == 1,
                  "2947 AC5: query deny-mailbox-hold-slo-total");
            CHECK(href(cs, "force-reason-code") ==
                      static_cast<std::int64_t>(SecurityScheduleForceReason::mailbox_hold_slo),
                  "2947 AC5: query force-reason-code = mailbox_hold_slo");
            CHECK(href(cs, "schema-2947") == 2947, "2947 AC5: schema-2947");
            CHECK(href(cs, "issue-2947") == 2947, "2947 AC5: issue-2947");
            CHECK(href(cs, "security-schedule-mailbox-hold-slo-wired") == 1,
                  "2947 AC5: wired sentinel");
            CHECK(href(cs, "schema-2590") == 2590, "2947 AC5: schema-2590 preserved");

            const auto gate_h = read_file("src/orch/security_schedule_gate.h");
            CHECK(gate_h.find("mailbox_hold_slo") != std::string::npos,
                  "2947 AC6: force reason enum");
            CHECK(gate_h.find("mailbox_wait_p99_us") != std::string::npos,
                  "2947 AC6: input field p99");
            CHECK(gate_h.find("fill_mailbox_hold_slo_live_") != std::string::npos,
                  "2947 AC6: live fill helper");
            CHECK(gate_h.find("Issue #2947") != std::string::npos ||
                      gate_h.find("#2947") != std::string::npos,
                  "2947 AC6: cites #2947");
            const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
            CHECK(prim.find("deny-mailbox-hold-slo-total") != std::string::npos,
                  "2947 AC6: query key in prims");
            CHECK(prim.find("schema-2947") != std::string::npos, "2947 AC6: schema-2947 in prims");
            const auto build = read_file("build.py");
            CHECK(build.find("check_mailbox_hold_slo_security_schedule_2947") != std::string::npos,
                  "2947 AC6: build.py wires linter");
            const auto readme = read_file("src/orch/README.md");
            CHECK(readme.find("mailbox_hold_slo") != std::string::npos ||
                      readme.find("mailbox-hold-slo") != std::string::npos,
                  "2947 AC6: README documents mailbox_hold_slo");
            // #2587 still independent
            const auto mbc = read_file("src/compiler/evaluator_mutation_boundary.cpp");
            CHECK(mbc.find("aura_mailbox_starvation_throttled") != std::string::npos ||
                      mbc.find("aura_orch_mailbox_starvation_throttled") != std::string::npos,
                  "2947 AC6: #2587 starvation gate still present (defense-in-depth)");
            std::ifstream invent("tests/orch/test_issue_2947.cpp");
            if (!invent.good())
                invent.open("../tests/orch/test_issue_2947.cpp");
            CHECK(!invent.good(), "2947 AC6: no test_issue_2947.cpp");
            CHECK(read_file("docs/design/2947-mailbox-hold-slo.md").empty(),
                  "2947 AC6: no docs/design/ per #1655");
        }
    }

    // ─── Issue #3211: production WAL append-fail SLO → schedule deny ───
    // AC1: input additive + pure decide deny with wal-append-fail-breach
    // AC2: production + would_arm → admit reject; Soft never rejects
    // AC3: live helper + boundary uses make_security_schedule_input_live
    // AC4: query wal-append-fail-breach / would-deny-admit / schema-3211
    // AC5: consecutive==0 && combined==0 → live helper false (two loads)
    // AC6: recover consecutive=0 → admit again; linter; no invent
    {
        std::println("\n--- #3211 AC1–AC6: WAL append-fail schedule deny ---");
        CHECK(aura::orch::kSecurityScheduleWalAppendFailIssue == 3211, "3211: issue stamp");

        // AC1 / AC2: production + wal_append_fail_would_arm → deny
        {
            reset_orch_security_schedule_counters_for_test();
            auto in = base_input();
            in.production_mode = true;
            in.soft_mode = false;
            in.wal_append_fail_would_arm = true;
            const auto d1 = decide_security_schedule(in);
            const auto d2 = decide_security_schedule(in);
            CHECK(d1.would_allow_new_mutate == d2.would_allow_new_mutate,
                  "3211 AC1: pure decide idempotent");
            CHECK(!d1.would_allow_new_mutate, "3211 AC2: production + would_arm → deny");
            CHECK(d1.force_reason == SecurityScheduleForceReason::wal_append_fail_breach,
                  "3211 AC2: force_reason = wal_append_fail_breach");
            CHECK(std::string(aura::orch::security_schedule_force_reason_name(d1.force_reason)) ==
                      "wal-append-fail-breach",
                  "3211 AC2: stable force_reason string");
            const auto d = evaluate_security_schedule(in);
            CHECK(!d.would_allow_new_mutate, "3211 AC2: evaluate denies");
            CHECK(g_orch_security_schedule_counters.deny_wal_append_fail_breach_total.load(
                      std::memory_order_relaxed) == 1,
                  "3211 AC2: deny_wal_append_fail_breach_total++");
            const auto rej = aura::orch::admit_security_schedule(in);
            CHECK(rej.has_value(), "3211 AC2: admit rejects under production");
            CHECK(rej.value_or("").find("wal-append-fail-breach") != std::string::npos,
                  "3211 AC2: admit reason = wal-append-fail-breach");
        }

        // AC2 Soft: same signal never hard-deny
        {
            reset_orch_security_schedule_counters_for_test();
            auto in = base_input();
            in.production_mode = true;
            in.soft_mode = true;
            in.wal_append_fail_would_arm = true;
            const auto d = evaluate_security_schedule(in);
            CHECK(d.would_allow_new_mutate, "3211 AC2: Soft never denies on WAL append-fail");
            CHECK(d.force_reason == SecurityScheduleForceReason::ok,
                  "3211 AC2: Soft force_reason ok");
            CHECK(g_orch_security_schedule_counters.deny_wal_append_fail_breach_total.load(
                      std::memory_order_relaxed) == 0,
                  "3211 AC2: Soft does not bump deny_wal_append_fail_breach_total");
            CHECK(!aura::orch::admit_security_schedule(in).has_value(),
                  "3211 AC2: Soft admit never rejects");
        }

        // AC4: commit_not_ready wins over wal append-fail
        {
            auto in = base_input();
            in.production_mode = true;
            in.commit_readiness_would_allow = false;
            in.commit_readiness_hard_reject = true;
            in.wal_append_fail_would_arm = true;
            const auto d = decide_security_schedule(in);
            CHECK(d.force_reason == SecurityScheduleForceReason::commit_not_ready,
                  "3211 AC4: does not mask commit_not_ready");
        }

        // AC5: quiet live helper (no consecutive / combined fail)
        {
            aura::core::wal_slo::reset_wal_append_fail_slo_for_test();
            CHECK(!aura::orch::wal_append_fail_would_arm_live(/*prod=*/true, /*soft=*/false),
                  "3211 AC5: quiet consecutive=0 → live false");
        }

        // AC6.1: inject consecutive SLO → live would_arm + admit deny
        {
            reset_orch_security_schedule_counters_for_test();
            aura::core::wal_slo::reset_wal_append_fail_slo_for_test();
            aura::core::wal_slo::g_wal_append_fail_slo_counters.consecutive.store(
                aura::core::wal_slo::wal_append_fail_slo_consecutive(), std::memory_order_relaxed);
            aura::core::wal_slo::g_wal_append_fail_slo_counters.combined_fail_total.store(
                3, std::memory_order_relaxed);
            CHECK(aura::orch::wal_append_fail_would_arm_live(/*prod=*/true, /*soft=*/false),
                  "3211 AC6: consecutive SLO + production → would_arm");
            CHECK(!aura::orch::wal_append_fail_would_arm_live(/*prod=*/true, /*soft=*/true),
                  "3211 AC6: Soft live helper never arms");
            const auto live_in = aura::orch::make_security_schedule_input_live(
                /*sandbox Off=*/0, /*prod=*/true, /*soft=*/false);
            CHECK(live_in.wal_append_fail_would_arm, "3211 AC6: live input would_arm");
            // Admit via a clean input so leftover commit_readiness / deny-storm
            // from earlier batch members cannot mask the WAL reason.
            auto in = base_input();
            in.production_mode = true;
            in.wal_append_fail_would_arm = true;
            const auto rej = aura::orch::admit_security_schedule(in);
            CHECK(rej.has_value(), "3211 AC6: next mutate schedule-denied");
            CHECK(rej.value_or("").find("wal-append-fail-breach") != std::string::npos,
                  "3211 AC6: deny reason wal-append-fail-breach");
            CHECK(href(cs, "wal-append-fail-breach") == 1,
                  "3211 AC4: query wal-append-fail-breach");
            CHECK(href(cs, "would-deny-admit") == 1, "3211 AC4: query would-deny-admit");
            CHECK(href(cs, "schema-3211") == 3211, "3211 AC4: schema-3211");
            CHECK(href(cs, "issue-3211") == 3211, "3211 AC4: issue-3211");
            CHECK(href(cs, "security-schedule-wal-append-fail-wired") == 1,
                  "3211 AC4: wired sentinel");
            CHECK(href(cs, "schema-2590") == 2590, "3211 AC4: schema-2590 preserved");
        }

        // AC6.3: clear consecutive → admit again
        {
            reset_orch_security_schedule_counters_for_test();
            aura::core::wal_slo::reset_wal_append_fail_slo_for_test();
            CHECK(!aura::orch::wal_append_fail_would_arm_live(/*prod=*/true, /*soft=*/false),
                  "3211 AC6: recovered consecutive=0 → not armed");
            auto in = base_input();
            in.production_mode = true;
            in.wal_append_fail_would_arm = false;
            CHECK(!aura::orch::admit_security_schedule(in).has_value(),
                  "3211 AC6: recovered admit allows");
        }

        // AC6 source-cite / no invent
        {
            const auto gate_h = read_file("src/orch/security_schedule_gate.h");
            CHECK(gate_h.find("wal_append_fail_would_arm") != std::string::npos,
                  "3211 AC6: input field");
            CHECK(gate_h.find("wal_append_fail_would_arm_live") != std::string::npos,
                  "3211 AC6: live helper");
            CHECK(gate_h.find("wal-append-fail-breach") != std::string::npos,
                  "3211 AC6: force_reason string");
            CHECK(gate_h.find("make_security_schedule_input_live") != std::string::npos,
                  "3211 AC3: existing live helper still used");
            const auto mbc = read_file("src/compiler/evaluator_mutation_boundary.cpp");
            CHECK(mbc.find("wal_append_fail_would_arm_live") != std::string::npos,
                  "3211 AC3: boundary cites live helper");
            const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
            CHECK(prim.find("wal-append-fail-breach") != std::string::npos, "3211 AC4: query key");
            CHECK(prim.find("would-deny-admit") != std::string::npos, "3211 AC4: would-deny-admit");
            const auto build = read_file("build.py");
            CHECK(build.find("check_wal_append_fail_schedule_3211") != std::string::npos,
                  "3211 AC6: build.py wires linter");
            CHECK(read_file("docs/design/3211-wal-append-fail-schedule.md").empty(),
                  "3211 AC6: no docs/design/ per #1655");
            CHECK(read_file("tests/orch/test_issue_3211.cpp").empty(),
                  "3211 AC6: no test_issue_3211.cpp per #81967");
        }
    }

    // ─── Issue #3244: production metrics hash overflow → observe+posture ───
    // AC1: input additive + live helper from existing overflow counters
    // AC2: production + would_arm → force_reason, would_allow stays true;
    //      Soft never arms / never denies
    // AC3: make_security_schedule_input_live fills the signal
    // AC4: query metrics-hash-overflow-breach / schema-3244; schema-2590 kept
    // AC5: overflow totals 0 → live helper false without extra bus
    {
        using aura::orch::kSecurityScheduleMetricsHashOverflowIssue;
        using aura::orch::metrics_hash_overflow_would_arm_live;
        std::println("\n--- #3244 AC1–AC5: metrics hash overflow schedule observe ---");
        CHECK(kSecurityScheduleMetricsHashOverflowIssue == 3244, "3244: issue stamp");

        {
            reset_orch_security_schedule_counters_for_test();
            auto in = base_input();
            in.production_mode = true;
            in.soft_mode = false;
            in.metrics_hash_overflow_would_arm = true;
            const auto d1 = decide_security_schedule(in);
            const auto d2 = decide_security_schedule(in);
            CHECK(d1.would_allow_new_mutate == d2.would_allow_new_mutate,
                  "3244 AC1: pure decide idempotent");
            CHECK(d1.would_allow_new_mutate, "3244 AC2: overflow does not deny admit");
            CHECK(d1.force_reason == SecurityScheduleForceReason::metrics_hash_overflow_breach,
                  "3244 AC2: force_reason = metrics-hash-overflow-breach");
            CHECK(std::string(aura::orch::security_schedule_force_reason_name(d1.force_reason)) ==
                      "metrics-hash-overflow-breach",
                  "3244 AC1: stable force_reason string");
            const auto d = evaluate_security_schedule(in);
            CHECK(d.would_allow_new_mutate, "3244 AC2: evaluate still allows");
            CHECK(g_orch_security_schedule_counters.observe_metrics_hash_overflow_total.load(
                      std::memory_order_relaxed) == 1,
                  "3244 AC2: observe_metrics_hash_overflow_total++");
            CHECK(g_orch_security_schedule_counters.deny_total.load(std::memory_order_relaxed) == 0,
                  "3244 AC2: deny_total unchanged");
            CHECK(!aura::orch::admit_security_schedule(in).has_value(),
                  "3244 AC2: admit does not reject on overflow (observe first)");
            CHECK(href(cs, "metrics-hash-overflow-breach") == 1,
                  "3244 AC4: query metrics-hash-overflow-breach");
            CHECK(href(cs, "would-deny-admit") == 0, "3244 AC4: query would-deny-admit=0");
            CHECK(href(cs, "schema-3244") == 3244, "3244 AC4: schema-3244");
            CHECK(href(cs, "issue-3244") == 3244, "3244 AC4: issue-3244");
            CHECK(href(cs, "security-schedule-metrics-hash-overflow-wired") == 1,
                  "3244 AC4: wired sentinel");
            CHECK(href(cs, "schema-2590") == 2590, "3244 AC4: schema-2590 preserved");
        }

        {
            reset_orch_security_schedule_counters_for_test();
            auto in = base_input();
            in.production_mode = true;
            in.soft_mode = true;
            in.metrics_hash_overflow_would_arm = true;
            const auto d = evaluate_security_schedule(in);
            CHECK(d.would_allow_new_mutate, "3244 AC2: Soft never denies");
            CHECK(d.force_reason == SecurityScheduleForceReason::ok,
                  "3244 AC2: Soft force_reason ok");
            CHECK(g_orch_security_schedule_counters.observe_metrics_hash_overflow_total.load(
                      std::memory_order_relaxed) == 0,
                  "3244 AC2: Soft does not bump observe counter");
        }

        {
            auto in = base_input();
            in.production_mode = true;
            in.commit_readiness_would_allow = false;
            in.commit_readiness_hard_reject = true;
            in.metrics_hash_overflow_would_arm = true;
            const auto d = decide_security_schedule(in);
            CHECK(d.force_reason == SecurityScheduleForceReason::commit_not_ready,
                  "3244 AC2: overflow does not mask commit_not_ready");
            CHECK(!d.would_allow_new_mutate, "3244 AC2: real deny still denies");
        }

        {
            aura_engine_metrics_reset_hash_overflow_for_test();
            aura_query_hash_reset_overflow_for_test();
            CHECK(!metrics_hash_overflow_would_arm_live(/*prod=*/true, /*soft=*/false),
                  "3244 AC5: quiet overflow=0 → live false");
            CHECK(!metrics_hash_overflow_would_arm_live(/*prod=*/false, /*soft=*/false),
                  "3244 AC2: Soft/non-prod live helper never arms");
            aura_query_hash_set_force_cap(4);
            (void)cs.eval("(engine:metrics \"query:security-posture\")");
            aura_query_hash_set_force_cap(0);
            CHECK(metrics_hash_overflow_would_arm_live(/*prod=*/true, /*soft=*/false),
                  "3244 AC3: production live helper arms after overflow");
            CHECK(!metrics_hash_overflow_would_arm_live(/*prod=*/true, /*soft=*/true),
                  "3244 AC2: Soft live helper never arms after overflow");
            const auto live_in = aura::orch::make_security_schedule_input_live(
                /*sandbox Off=*/0, /*prod=*/true, /*soft=*/false);
            CHECK(live_in.metrics_hash_overflow_would_arm, "3244 AC3: live input would_arm");
            aura_query_hash_reset_overflow_for_test();
        }

        {
            const auto gate_h = read_file("src/orch/security_schedule_gate.h");
            CHECK(gate_h.find("metrics_hash_overflow_would_arm") != std::string::npos,
                  "3244 AC1: input field");
            CHECK(gate_h.find("metrics_hash_overflow_would_arm_live") != std::string::npos,
                  "3244 AC3: live helper");
            CHECK(gate_h.find("metrics-hash-overflow-breach") != std::string::npos,
                  "3244 AC1: force_reason string");
            const auto mbc = read_file("src/compiler/evaluator_mutation_boundary.cpp");
            CHECK(mbc.find("metrics_hash_overflow_would_arm_live") != std::string::npos,
                  "3244 AC3: boundary cites live helper");
            const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
            CHECK(prim.find("metrics-hash-overflow-breach") != std::string::npos,
                  "3244 AC4: posture/query key");
            CHECK(prim.find("schema-3244") != std::string::npos, "3244 AC4: schema-3244");
            CHECK(prim.find("schema-2590") != std::string::npos, "3244 AC4: schema-2590 preserved");
            const auto build = read_file("build.py");
            CHECK(build.find("check_metrics_hash_overflow_posture_3244") != std::string::npos,
                  "3244 AC5: build.py wires linter");
            CHECK(read_file("docs/design/3244-metrics-hash-overflow-posture.md").empty(),
                  "3244 AC5: no docs/design/ per #1655");
            CHECK(read_file("tests/compiler/test_issue_3244.cpp").empty(),
                  "3244 AC5: no test_issue_3244.cpp per #81967");
        }
    }

    {
        std::println("\n--- #3251: schedule-gate body deny-class ---");
        using aura::orch::AgentDenyClass;
        using aura::orch::AgentSpec;
        using aura::orch::g_capability_deny_storm_threshold;
        using aura::orch::join_agent;
        using aura::orch::JoinPolicy;
        using aura::orch::spawn_agent_with_mailbox;
        using aura::serve::SchedRunner;
        using aura::serve::Scheduler;
        aura::compiler::typed_audit::apply_production_audit_defaults();
        const auto prev_thr =
            g_capability_deny_storm_threshold().exchange(0, std::memory_order_relaxed);
        Scheduler sched(1);
        SchedRunner runner(sched);
        AgentSpec spec;
        spec.name = "3251-sched";
        spec.attach_mailbox = false;
        spec.body = [] {};
        auto h = spawn_agent_with_mailbox(sched, spec);
        CHECK(h.ok, "3251: spawn ok (deny is body try_acquire)");
        JoinPolicy jp{};
        jp.primary_ms = 2000;
        jp.drain_ms = 200;
        (void)join_agent(h, jp);
        CHECK(h.body_acquire_rejected(), "3251: body try_acquire rejected");
        CHECK(h.body_deny_class() == AgentDenyClass::ScheduleGate,
              "3251: deny class schedule-gate");
        g_capability_deny_storm_threshold().store(prev_thr, std::memory_order_relaxed);
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        reset_orch_security_schedule_counters_for_test();
    }

    reset_orch_security_schedule_counters_for_test();
    aura::core::wal_slo::reset_wal_append_fail_slo_for_test();
    std::println("\n=== #2590/#2947/#3211/#3244/#3251: {}/{} checks passed ===", g_passed,
                 g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_security_schedule_gate();
}
#endif
