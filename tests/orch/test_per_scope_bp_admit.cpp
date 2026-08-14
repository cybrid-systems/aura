// tests/orch/test_per_scope_bp_admit.cpp
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
//   - tests/orch/test_per_scope_bp_admit.cpp (this file).
//
// No docs/design/ per #1655 / #1485.

#include "test_harness.hpp"

#include "compiler/typed_mutation_audit.h"
#include "orch/agent_scope.h"
#include "orch/agent_spawn.h"
#include "serve/scheduler.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
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

int run_test_per_scope_bp_admit() {
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

    // ── Issue #2948: SSOT resolve_bp_threshold (spawn face) ──────
    {
        std::println("\n--- #2948 AC1/AC3/AC6: resolve_bp_threshold spawn semantics ---");
        CHECK(aura::orch::kBpThresholdSsotIssue == 2948, "2948: issue stamp");

        // AC1: nullopt + empty scope → same as resolve_mailbox_bp_admit_threshold
        {
            unsetenv("AURA_ORCH_BP_ADMIT_THRESHOLD");
            const auto proc = aura::orch::resolve_mailbox_bp_admit_threshold();
            const auto d = aura::orch::resolve_bp_threshold(
                std::nullopt, /*scope_id=*/{}, /*policy_zero_means_process_default=*/false);
            CHECK(d.threshold == proc, "2948 AC1: nullopt threshold == process default");
            CHECK(d.using_process_default, "2948 AC1: using_process_default");
            CHECK(!d.always_reject, "2948 AC1: not always_reject");
            CHECK(std::string_view{d.source} == "process" || std::string_view{d.source} == "off",
                  "2948 AC1: source process|off");
        }

        // AC3: spec 0 → always_reject (distinct from process env=0)
        {
            unsetenv("AURA_ORCH_BP_ADMIT_THRESHOLD");
            const auto d = aura::orch::resolve_bp_threshold(
                std::optional<std::uint64_t>{0}, {}, /*policy_zero_means_process_default=*/false);
            CHECK(d.always_reject, "2948 AC3: spec 0 → always_reject");
            CHECK(std::string_view{d.source} == "spec-admit-off",
                  "2948 AC3: source=spec-admit-off");
            CHECK(d.override_active, "2948 AC3: override_active for counter routing");
        }

        // Policy flag: 0 → process default (not always_reject)
        {
            const auto proc = aura::orch::resolve_mailbox_bp_admit_threshold();
            const auto d = aura::orch::resolve_bp_threshold(
                std::optional<std::uint64_t>{0}, {}, /*policy_zero_means_process_default=*/true);
            CHECK(!d.always_reject, "2948 AC3: policy 0 is NOT always_reject");
            CHECK(d.threshold == proc, "2948 AC3: policy 0 → process threshold");
            CHECK(d.using_process_default, "2948 AC3: policy 0 using_process_default");
        }

        // Spec N>0
        {
            const auto d =
                aura::orch::resolve_bp_threshold(std::optional<std::uint64_t>{64}, {}, false);
            CHECK(d.threshold == 64, "2948: spec N → threshold N");
            CHECK(std::string_view{d.source} == "spec-override", "2948: source=spec-override");
            CHECK(!d.always_reject, "2948: N not always_reject");
        }

        // AC4: load_mailbox_bp_recent scope isolation
        {
            reset_all();
            aura::orch::note_mailbox_bp_recent_event("tenant-ssot-a");
            aura::orch::note_mailbox_bp_recent_event("tenant-ssot-a");
            aura::orch::note_mailbox_bp_recent_event("tenant-ssot-a");
            const auto a = aura::orch::load_mailbox_bp_recent("tenant-ssot-a");
            const auto b = aura::orch::load_mailbox_bp_recent("tenant-ssot-b");
            CHECK(a >= 3, "2948 AC4: scope A recent loaded");
            CHECK(b == 0, "2948 AC4: scope B clean (same helper as spawn)");
            (void)aura::orch::erase_scope_bp_gauge("tenant-ssot-a");
        }

        // Source-cite
        {
            auto spawn_h = read_file("src/orch/agent_spawn.h");
            auto scope_h = read_file("src/orch/agent_scope.h");
            auto build = read_file("build.py");
            CHECK(spawn_h.find("resolve_bp_threshold") != std::string::npos,
                  "2948 AC6: resolve_bp_threshold present");
            CHECK(spawn_h.find("load_mailbox_bp_recent") != std::string::npos,
                  "2948 AC6: load_mailbox_bp_recent present");
            CHECK(spawn_h.find("spec-admit-off") != std::string::npos,
                  "2948 AC6: documents spec-admit-off");
            CHECK(spawn_h.find("policy_zero_means_process_default") != std::string::npos,
                  "2948 AC6: policy_zero flag");
            CHECK(scope_h.find("resolve_bp_threshold") != std::string::npos,
                  "2948 AC6: watch_all uses resolve_bp_threshold");
            CHECK(build.find("check_bp_threshold_ssot_2948") != std::string::npos,
                  "2948 AC6: build.py wires linter");
            CHECK(read_file("docs/design/2948-bp-threshold-ssot.md").empty(),
                  "2948 AC6: no docs/design/");
        }
    }

    // ── Issue #3015: production AgentScope inherit (no cross-poison) ──
    {
        std::println("\n--- #3015 AC: production scope inherit / Soft process bucket ---");
        CHECK(aura::orch::kBpScopeInheritIssue == 3015, "#3015 AC1: issue stamp");
        CHECK(aura::orch::kBpScopeProcessBucket == "-", "#3015 AC1: process-bucket sentinel");

        auto set_prod = [](bool on) {
            aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
                .store(on ? 1u : 0u, std::memory_order_relaxed);
        };

        auto reset_3015 = [&]() {
            reset_all();
            g_orch_module_stats.spawn_bp_admit_reject_scope_total.store(0,
                                                                        std::memory_order_relaxed);
            g_orch_module_stats.spawn_bp_scope_inherited_total.store(0, std::memory_order_relaxed);
            g_orch_module_stats.spawn_bp_process_bucket_used_total.store(0,
                                                                         std::memory_order_relaxed);
            (void)aura::orch::reset_scope_bp_map_for_test();
        };

        {
            const auto scope_h = read_file("src/orch/agent_scope.h");
            const auto spawn_h = read_file("src/orch/agent_spawn.h");
            const auto readme = read_file("src/orch/README.md");
            CHECK(scope_h.find("production_scope_bp_inherit") != std::string::npos,
                  "#3015 AC1: inherit helper");
            CHECK(scope_h.find("Issue #3015") != std::string::npos, "#3015 AC1: scope cites #3015");
            CHECK(spawn_h.find("kBpScopeProcessBucket") != std::string::npos,
                  "#3015 AC1: process-bucket sentinel");
            CHECK(readme.find("Issue #3015") != std::string::npos,
                  "#3015 AC1: README documents inherit");
            CHECK(readme.find("as:<seq>") != std::string::npos,
                  "#3015 AC1: README session-local key");
            CHECK(read_file("docs/design/3015-bp-scope-inherit.md").empty(),
                  "#3015: no docs/design/");
        }

        {
            reset_3015();
            set_prod(false);
            unsetenv("AURA_SANDBOX");
            CHECK(!aura::orch::production_scope_bp_inherit(), "#3015 AC2: Soft inherit OFF");
            aura::orch::AgentScope sa(sched);
            aura::orch::AgentScope sb(sched);
            CHECK(sa.bp_scope_id() != sb.bp_scope_id(), "#3015 AC2: scopes have distinct keys");
            const auto inh0 =
                g_orch_module_stats.spawn_bp_scope_inherited_total.load(std::memory_order_relaxed);
            auto spec = make_spec("soft-a");
            auto& ha = sa.spawn(std::move(spec));
            CHECK(ha.ok, "#3015 AC2: Soft scope spawn admits (no storm)");
            CHECK(g_orch_module_stats.spawn_bp_scope_inherited_total.load(
                      std::memory_order_relaxed) == inh0,
                  "#3015 AC2: Soft does not inherit");
        }

        {
            reset_3015();
            set_prod(true);
            unsetenv("AURA_SANDBOX");
            CHECK(aura::orch::production_scope_bp_inherit(), "#3015 AC3: production inherit ON");
            aura::orch::AgentScope sa(sched);
            aura::orch::AgentScope sb(sched);
            const auto inh0 =
                g_orch_module_stats.spawn_bp_scope_inherited_total.load(std::memory_order_relaxed);
            const auto proc_rej0 =
                g_orch_module_stats.spawn_bp_admit_reject_total.load(std::memory_order_relaxed);
            const auto scope_rej0 = g_orch_module_stats.spawn_bp_admit_reject_scope_total.load(
                std::memory_order_relaxed);

            auto spec_a0 = make_spec("prod-a0");
            auto& ha0 = sa.spawn(std::move(spec_a0));
            CHECK(ha0.ok, "#3015 AC3: first A spawn admits");
            CHECK(g_orch_module_stats.spawn_bp_scope_inherited_total.load(
                      std::memory_order_relaxed) == inh0 + 1,
                  "#3015 AC3: inherit bumped");

            for (int i = 0; i < 40; ++i)
                note_mailbox_bp_recent_event(sa.bp_scope_id());
            bump_bp_recent(40);

            auto spec_b = make_spec("prod-b");
            auto& hb = sb.spawn(std::move(spec_b));
            CHECK(hb.ok, "#3015 AC3: B admits while A + process bucket are hot");
            CHECK(g_orch_module_stats.spawn_bp_admit_reject_total.load(std::memory_order_relaxed) ==
                      proc_rej0,
                  "#3015 AC3: process spawn_bp_admit_reject unchanged for B");

            auto spec_a1 = make_spec("prod-a1");
            auto& ha1 = sa.spawn(std::move(spec_a1));
            CHECK(!ha1.ok, "#3015 AC3: A rejects on its own storm");
            CHECK(ha1.quota_dimension == "mailbox-bp", "#3015 AC3: A reject is mailbox-bp");
            CHECK(g_orch_module_stats.spawn_bp_admit_reject_scope_total.load(
                      std::memory_order_relaxed) == scope_rej0 + 1,
                  "#3015 AC3: reject counted as scope, not process");
            CHECK(g_orch_module_stats.spawn_bp_admit_reject_total.load(std::memory_order_relaxed) ==
                      proc_rej0,
                  "#3015 AC3: process reject counter still unchanged");
        }

        {
            reset_3015();
            set_prod(true);
            unsetenv("AURA_SANDBOX");
            bump_bp_recent(40);
            aura::orch::AgentScope sc(sched);
            auto spec = make_spec("explicit-process");
            spec.bp_scope_id = std::string{aura::orch::kBpScopeProcessBucket};
            auto& h = sc.spawn(std::move(spec));
            CHECK(!h.ok, "#3015 AC2: explicit '-' uses process bucket (hot → reject)");
            CHECK(h.quota_dimension == "mailbox-bp", "#3015 AC2: '-' reject mailbox-bp");
        }

        {
            reset_3015();
            set_prod(true);
            unsetenv("AURA_SANDBOX");
            auto spec = make_spec("direct-prod");
            auto h = aura::orch::spawn_agent_with_mailbox(sched, std::move(spec));
            CHECK(h.ok, "#3015 AC2: direct empty id admits (no storm)");
            CHECK(g_orch_module_stats.spawn_bp_process_bucket_used_total.load(
                      std::memory_order_relaxed) >= 1,
                  "#3015 AC1: production process-bucket use is observable");
        }

        set_prod(false);
        unsetenv("AURA_SANDBOX");
        reset_3015();
    }

    g_orch_module_stats.mailbox_bp_recent_total.store(0, std::memory_order_relaxed);
    g_orch_module_stats.spawn_bp_admit_reject_total.store(0, std::memory_order_relaxed);
    g_orch_module_stats.spawn_bp_admit_reject_override_total.store(0, std::memory_order_relaxed);
    std::println("\n=== #2591/#2948/#3015: {}/{} checks passed ===", g_passed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_per_scope_bp_admit();
}
#endif
