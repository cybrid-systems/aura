// tests/orch/test_per_scope_bp_admit_2591.cpp
// @category: integration
// @reason: Issue #2591 — per-spec BP admit threshold override
//          (multi-tenant / multi-scope isolation). AgentSpec gains
//          `bp_admit_threshold` (optional<uint64_t>); nullopt → process
//          default (#2535 default=32, env override via
//          AURA_ORCH_BP_ADMIT_THRESHOLD); 0 → admit off for THIS
//          spawn (always reject under attach_mailbox); N > 0 → local
//          threshold (per-spawn policy isolation, gauge is still
//          process-global). Wire surface: Aura kwarg
//          `:bp-admit-threshold n` on (orch:spawn-agent ...).
//
//   AC1: Spec-level threshold overrides process default without env
//        change.
//   AC2: threshold=0 on one spawn does not disable process default for
//        others.
//   AC3: Default path (no override) unchanged (#2535 production mild
//        gate).
//   AC4: Structured reject still quota_dimension="mailbox-bp" +
//        reserved=0 (no-leak #2155).
//   AC5: spawn_bp_admit_reject_override_total bumps for override
//        rejects; spawn_bp_admit_reject_total bumps for default
//        rejects (separate counters so multi-tenant dashboards can
//        distinguish).
//
// Source-cite (issue #2591):
//   - src/orch/agent_spawn.h: AgentSpec::bp_admit_threshold (optional);
//     preflight in spawn_agent_with_mailbox uses spec override OR
//     process default; spawn_bp_admit_reject_override_total counter
//     (separate from spawn_bp_admit_reject_total).
//   - src/compiler/evaluator_primitives_agent.cpp: orch:spawn-agent
//     parses :bp-admit-threshold n kwarg and sets
//     spec.bp_admit_threshold.
//   - src/orch/README.md: "Per-spec override (Issue #2591, ...)"
//     sub-section under "Mailbox BP admission".
//   - tests/orch/test_per_scope_bp_admit_2591.cpp (this file).
//
// No docs/design/ per #1655 / #1485.

#include "test_harness.hpp"

#include "orch/agent_spawn.h"
#include "serve/scheduler.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;

namespace {

using aura::orch::AgentSpec;
using aura::orch::g_orch_module_stats;
using aura::orch::note_mailbox_bp_recent_event;
using aura::serve::Scheduler;
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

static void bump_bp_recent(std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; ++i) {
        note_mailbox_bp_recent_event();
    }
}

} // namespace

int run_test_per_scope_bp_admit_2591() {
    std::println("=== Issue #2591: per-spec BP admit threshold override ===");

    // ── README source-cite ──
    {
        std::println("\n--- #2591 README source-cite ---");
        const auto readme_src = read_file("src/orch/README.md");
        CHECK(readme_src.find("Per-spec override") != std::string::npos,
              "AC5: README has 'Per-spec override' sub-section");
        CHECK(readme_src.find("Issue #2591") != std::string::npos, "AC5: README cites Issue #2591");
        CHECK(readme_src.find("bp-admit-threshold") != std::string::npos,
              "AC5: README cites Aura kwarg :bp-admit-threshold");
        CHECK(readme_src.find("spawn_bp_admit_reject_override_total") != std::string::npos,
              "AC5: README cites spawn_bp_admit_reject_override_total counter");
        CHECK(readme_src.find("quota_dimension=\"mailbox-bp\"") != std::string::npos,
              "AC5: README preserves quota_dimension=mailbox-bp contract");
    }

    Scheduler sched;

    auto make_spec = [](const std::string& name) {
        AgentSpec spec;
        spec.name = name;
        spec.attach_mailbox = true;
        spec.body = []() { /* no-op */ };
        return spec;
    };

    auto reset_all = []() {
        // No public reset_orch_module_stats_for_test yet — zero the gauges
        // this suite reads so each AC starts from a stable baseline.
        g_orch_module_stats.mailbox_bp_recent_total.store(0, std::memory_order_relaxed);
        g_orch_module_stats.spawn_bp_admit_reject_total.store(0, std::memory_order_relaxed);
        g_orch_module_stats.spawn_bp_admit_reject_override_total.store(0,
                                                                       std::memory_order_relaxed);
    };

    // ── AC3: Default path (no override) — process default (kMailboxBpAdmitThresholdDefault=32) ──
    {
        std::println("\n--- #2591 AC3: default path unchanged ---");
        reset_all();
        // bp_recent=10 < 32 → default path admits.
        bump_bp_recent(10);
        auto spec = make_spec("default-low");
        // No override (nullopt) → process default.
        CHECK(!spec.bp_admit_threshold.has_value(),
              "AC3: AgentSpec::bp_admit_threshold default-constructed = nullopt");
        auto h = aura::orch::spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok, "AC3: default path admits when bp_recent=10 < process default 32");
        CHECK(!h.quota_exceeded, "AC3: default path quota_exceeded=false when admitted");
    }

    // ── AC1: Spec-level override beats process default (override > bp_recent) ──
    {
        std::println("\n--- #2591 AC1: override beats process default ---");
        reset_all();
        // bp_recent=50 > process default 32 → default would reject.
        // With override=64 → admits (override beats default).
        bump_bp_recent(50);
        const auto reject_before =
            g_orch_module_stats.spawn_bp_admit_reject_total.load(std::memory_order_relaxed);
        const auto override_before = g_orch_module_stats.spawn_bp_admit_reject_override_total.load(
            std::memory_order_relaxed);

        auto spec = make_spec("override-relaxed");
        spec.bp_admit_threshold = std::optional<std::uint64_t>{64};
        CHECK(spec.bp_admit_threshold.has_value(), "AC1: override set on spec");
        CHECK(*spec.bp_admit_threshold == 64, "AC1: override value 64");

        auto h = aura::orch::spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(h.ok, "AC1: override=64 admits when bp_recent=50 (process default would reject)");
        CHECK(!h.quota_exceeded, "AC1: override admit → quota_exceeded=false");
        // Counters must NOT have bumped (no reject).
        CHECK(g_orch_module_stats.spawn_bp_admit_reject_total.load(std::memory_order_relaxed) ==
                  reject_before,
              "AC1: process-default reject counter unchanged on override admit");
        CHECK(g_orch_module_stats.spawn_bp_admit_reject_override_total.load(
                  std::memory_order_relaxed) == override_before,
              "AC1: override reject counter unchanged on override admit");
    }

    // ── AC4 + AC5: Override < bp_recent → reject + structured reject + counter ──
    {
        std::println("\n--- #2591 AC4 + AC5: override reject with structured fields ---");
        reset_all();
        bump_bp_recent(50);
        const auto override_before = g_orch_module_stats.spawn_bp_admit_reject_override_total.load(
            std::memory_order_relaxed);

        auto spec = make_spec("override-strict");
        spec.bp_admit_threshold = std::optional<std::uint64_t>{32};

        auto h = aura::orch::spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(!h.ok, "AC4: override=32 rejects when bp_recent=50");
        CHECK(h.quota_exceeded, "AC4: override reject → quota_exceeded=true");
        CHECK(h.quota_dimension == "mailbox-bp",
              "AC4: override reject → quota_dimension=\"mailbox-bp\" (no-leak contract)");
        CHECK(h.reserved_memory_bytes == 0,
              "AC4: override reject → reserved_memory_bytes=0 (no-leak #2155)");
        // Counter bumped (override_active).
        CHECK(g_orch_module_stats.spawn_bp_admit_reject_override_total.load(
                  std::memory_order_relaxed) == override_before + 1,
              "AC5: spawn_bp_admit_reject_override_total++ on override reject");
        // Process default counter NOT bumped (override_active=true branches).
        CHECK(g_orch_module_stats.spawn_bp_admit_reject_total.load(std::memory_order_relaxed) == 0,
              "AC5: spawn_bp_admit_reject_total NOT bumped on override reject");
    }

    // ── AC2: override=0 on one spawn does NOT disable process default for others ──
    {
        std::println("\n--- #2591 AC2: override=0 isolation ---");
        reset_all();
        bump_bp_recent(50); // > 32 default; > 0 override

        // First spawn: override=0 → always reject (admit off for THIS spawn).
        auto spec_zero = make_spec("override-zero");
        spec_zero.bp_admit_threshold = std::optional<std::uint64_t>{0};
        auto h_zero = aura::orch::spawn_agent_with_mailbox(sched, std::move(spec_zero));
        CHECK(!h_zero.ok, "AC2: override=0 rejects when bp_recent=50 (admit off for this spawn)");

        // Second spawn: no override (nullopt) → process default STILL applies.
        // bp_recent=50 >= 32 → should reject (default policy unchanged).
        auto spec_default = make_spec("default-after-zero");
        // bp_admit_threshold left unset (nullopt) — uses process default.
        auto h_default = aura::orch::spawn_agent_with_mailbox(sched, std::move(spec_default));
        CHECK(!h_default.ok,
              "AC2: process default still rejects (override=0 didn't disable global policy)");
        CHECK(h_default.quota_dimension == "mailbox-bp",
              "AC2: default reject still quota_dimension=\"mailbox-bp\"");
        // spawn_bp_admit_reject_total should have bumped (default reject).
        CHECK(g_orch_module_stats.spawn_bp_admit_reject_total.load(std::memory_order_relaxed) >= 1,
              "AC2: process-default reject counter bumped (independent of override=0 spawn)");
    }

    // ── AC1 negative: override=0 with bp_recent=0 (no storm) → still rejects (admit off) ──
    {
        std::println("\n--- #2591 AC1 negative: override=0 with no storm ---");
        reset_all();
        // bp_recent=0 (no storm).
        // override=0 → admit off → always reject under attach_mailbox.
        auto spec = make_spec("override-zero-no-storm");
        spec.bp_admit_threshold = std::optional<std::uint64_t>{0};
        auto h = aura::orch::spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(!h.ok,
              "AC1 negative: override=0 rejects even with bp_recent=0 (admit off for this spawn)");
        CHECK(h.quota_exceeded, "AC1 negative: override=0 → quota_exceeded=true");
    }

    // ── Default counter separation: process-default reject bumps default counter, not override ──
    {
        std::println("\n--- #2591 AC5: counter separation under process-default reject ---");
        reset_all();
        bump_bp_recent(50); // > 32 default
        const auto default_before =
            g_orch_module_stats.spawn_bp_admit_reject_total.load(std::memory_order_relaxed);
        const auto override_before = g_orch_module_stats.spawn_bp_admit_reject_override_total.load(
            std::memory_order_relaxed);

        auto spec = make_spec("default-reject-only");
        // bp_admit_threshold left unset (nullopt) — process default applies.
        auto h = aura::orch::spawn_agent_with_mailbox(sched, std::move(spec));
        CHECK(!h.ok, "AC5: default reject when bp_recent > process threshold");
        CHECK(g_orch_module_stats.spawn_bp_admit_reject_total.load(std::memory_order_relaxed) ==
                  default_before + 1,
              "AC5: spawn_bp_admit_reject_total++ on default reject");
        CHECK(g_orch_module_stats.spawn_bp_admit_reject_override_total.load(
                  std::memory_order_relaxed) == override_before,
              "AC5: spawn_bp_admit_reject_override_total NOT bumped on default reject");
    }

    g_orch_module_stats.mailbox_bp_recent_total.store(0, std::memory_order_relaxed);
    g_orch_module_stats.spawn_bp_admit_reject_total.store(0, std::memory_order_relaxed);
    g_orch_module_stats.spawn_bp_admit_reject_override_total.store(0, std::memory_order_relaxed);
    std::println("\n=== #2591: {}/{} checks passed ===", g_passed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_per_scope_bp_admit_2591();
}
#endif
