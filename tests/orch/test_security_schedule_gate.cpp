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
    {
        std::println("\n--- #2660 AC4: #2587 mailbox starvation gate still independent ---");
        const auto mbc = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(mbc.find("aura_mailbox_starvation_throttled") != std::string::npos,
              "AC4: #2587 mailbox starvation gate still wired in try_acquire");
        CHECK(mbc.find("evaluate_security_schedule") != std::string::npos,
              "AC4: #2590/#2660 security-schedule gate still wired");
        // Both gates observable in the same site — order documented in
        // evaluator_mutation_boundary.cpp: #2587 first, then #2590/#2660.
        const auto mb_pos = mbc.find("aura_mailbox_starvation_throttled");
        const auto ss_pos = mbc.find("evaluate_security_schedule");
        CHECK(mb_pos < ss_pos,
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

    reset_orch_security_schedule_counters_for_test();
    std::println("\n=== #2590: {}/{} checks passed ===", g_passed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_security_schedule_gate();
}
#endif
