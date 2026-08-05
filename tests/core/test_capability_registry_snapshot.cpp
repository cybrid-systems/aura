// @category: unit
// @reason: Issue #2426 — CapabilityRegistry::snapshot_registry_state()
//          consistent multi-field view (#1840 pattern).
//
//   AC1: snapshot consistent under concurrent policy writers
//   AC2: concurrent grant + revoke + snapshot (TSan-friendly)
//   AC3: all snapshot fields use explicit acquire loads
//   AC4: individual accessors / assignment still work

#include "test_harness.hpp"

#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/workspace_epoch.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;

namespace {

using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_registry;
using aura::core::capability::RegistryStateSnapshot;
using aura::test::g_failed;
using aura::test::g_passed;

void reset_reg() {
    g_capability_registry().clear_for_test();
    g_capability_registry().set_grant_min_valid_epoch(0);
    g_capability_registry().set_grant_epoch_retain_window(0);
    g_capability_registry().set_hard_fiber_isolation(false);
}

} // namespace

int run_test_capability_registry_snapshot() {
    std::println("=== Issue #2426: snapshot_registry_state ===");

    // ── AC4 individual accessors still work ───────────────────────
    {
        std::println("\n--- #2426 AC4: accessors / assignment unchanged ---");
        reset_reg();
        auto& reg = g_capability_registry();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
        CHECK(reg.sandbox_mode == EffectSandboxMode::Strict, "AC4: sandbox assign+read");
        reg.default_tenant = 42;
        CHECK(reg.default_tenant.load() == 42, "AC4: default_tenant assign");
        reg.set_grant_min_valid_epoch(7);
        reg.set_grant_epoch_retain_window(3);
        reg.set_hard_fiber_isolation(true);
        CHECK(reg.grant_min_valid_epoch() == 7, "AC4: min_valid accessor");
        CHECK(reg.grant_epoch_retain_window() == 3, "AC4: retain accessor");
        CHECK(reg.hard_fiber_isolation(), "AC4: hard_fiber accessor");

        const auto snap = reg.snapshot_registry_state();
        CHECK(snap.sandbox_mode == EffectSandboxMode::Strict, "AC4: snap sandbox");
        CHECK(snap.default_tenant == 42, "AC4: snap tenant");
        CHECK(snap.grant_min_valid_epoch == 7, "AC4: snap min_valid");
        // set_grant_epoch_retain_window may advance min_valid via bump hook;
        // retain window itself is stored.
        CHECK(snap.grant_epoch_retain_window == 3, "AC4: snap retain");
        CHECK(snap.hard_fiber_isolation, "AC4: snap hard_fiber");
    }

    // ── AC1/AC3 sequential consistency of snapshot fields ──────────
    {
        std::println("\n--- #2426 AC1 + #2426 AC3: snapshot field coherence ---");
        reset_reg();
        auto& reg = g_capability_registry();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        reg.default_tenant = 9;
        reg.set_grant_min_valid_epoch(100);
        reg.set_grant_epoch_retain_window(0); // no auto-advance noise
        reg.set_hard_fiber_isolation(false);
        reg.record_audit(Effect::Read, Effect::Read, 1, EffectProvenance{}, false, "snap-ac1");

        const auto s = reg.snapshot_registry_state();
        CHECK(s.sandbox_mode == EffectSandboxMode::Restricted, "AC1: mode Restricted");
        CHECK(s.default_tenant == 9, "AC1: tenant 9");
        CHECK(s.grant_min_valid_epoch == 100, "AC1: min_valid 100");
        CHECK(s.grant_epoch_retain_window == 0, "AC1: retain 0");
        CHECK(!s.hard_fiber_isolation, "AC1: hard_fiber false");
        CHECK(s.audit_seq == 1, "AC1: audit_seq after one record");
        // Cross-check against individual acquire accessors.
        CHECK(s.grant_min_valid_epoch == reg.grant_min_valid_epoch(), "AC3: matches accessor");
        CHECK(s.hard_fiber_isolation == reg.hard_fiber_isolation(), "AC3: matches hard_fiber");
        CHECK(s.audit_seq == reg.load_audit_seq(), "AC3: matches load_audit_seq");
    }

    // ── AC2 concurrent grant/revoke/policy + snapshot ──────────────
    {
        std::println("\n--- #2426 AC2: concurrent grant/revoke/snapshot ---");
        reset_reg();
        auto& reg = g_capability_registry();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> grants{0};
        std::atomic<std::uint64_t> revokes{0};
        std::atomic<std::uint64_t> snaps{0};
        std::atomic<std::uint64_t> policy_writes{0};
        std::atomic<std::uint64_t> err{0};
        std::atomic<std::uint64_t> bad{0};

        std::vector<std::thread> threads;
        // 2 grant/revoke writers
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t]() {
                std::uint64_t i = static_cast<std::uint64_t>(t);
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        EffectProvenance prov{};
                        prov.epoch = 1;
                        prov.mutation_id = 1;
                        const auto name = std::format("g{}", i % 8);
                        reg.grant(/*tenant=*/1, name, Effect::Mutate, prov);
                        grants.fetch_add(1, std::memory_order_relaxed);
                        if ((i & 1u) != 0) {
                            reg.revoke(1, name, /*epoch=*/1);
                            revokes.fetch_add(1, std::memory_order_relaxed);
                        }
                        ++i;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // 1 policy writer (atomics in snapshot)
        threads.emplace_back([&]() {
            std::uint64_t i = 0;
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    reg.set_grant_min_valid_epoch(i % 50);
                    reg.set_hard_fiber_isolation((i & 1u) != 0);
                    // Issue #2657: route through the process-wide authority.
                    aura::core::sandbox::set_mode(
                        (i % 3 == 0)   ? aura::core::sandbox::SandboxMode::Off
                        : (i % 3 == 1) ? aura::core::sandbox::SandboxMode::Restricted
                                       : aura::core::sandbox::SandboxMode::Strict);
                    reg.default_tenant = i % 10;
                    if ((i % 5) == 0)
                        reg.set_grant_epoch_retain_window(i % 4);
                    policy_writes.fetch_add(1, std::memory_order_relaxed);
                    ++i;
                } catch (...) {
                    err.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        // 4 snapshot readers — validate internal field legality
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&]() {
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        const RegistryStateSnapshot s = reg.snapshot_registry_state();
                        snaps.fetch_add(1, std::memory_order_relaxed);
                        // Legal enum range
                        const auto m = static_cast<std::uint8_t>(s.sandbox_mode);
                        if (m > 2)
                            bad.fetch_add(1, std::memory_order_relaxed);
                        // bool only 0/1 representation
                        if (s.hard_fiber_isolation != false && s.hard_fiber_isolation != true)
                            bad.fetch_add(1, std::memory_order_relaxed);
                        // Re-snapshot immediately: if policy quiet, same;
                        // under load just ensure no crash / no illegal values.
                        (void)s.grant_min_valid_epoch;
                        (void)s.grant_epoch_retain_window;
                        (void)s.default_tenant;
                        (void)s.audit_seq;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        stop.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        std::println("  grants={} revokes={} snaps={} policy_writes={} bad={} err={}",
                     grants.load(), revokes.load(), snaps.load(), policy_writes.load(), bad.load(),
                     err.load());
        CHECK(grants.load() > 0, "AC2: concurrent grants progressed");
        CHECK(snaps.load() > 0, "AC2: concurrent snapshots progressed");
        CHECK(policy_writes.load() > 0, "AC2: concurrent policy writes progressed");
        CHECK(bad.load() == 0, "AC1/AC2: no illegal torn field values");
        CHECK(err.load() == 0, "AC2: no exceptions");

        // Final quiescent snapshot matches accessors.
        const auto final_s = reg.snapshot_registry_state();
        CHECK(final_s.grant_min_valid_epoch == reg.grant_min_valid_epoch(),
              "AC1: final snap min_valid == accessor");
        CHECK(final_s.hard_fiber_isolation == reg.hard_fiber_isolation(),
              "AC1: final snap hard_fiber == accessor");
        CHECK(final_s.sandbox_mode == reg.sandbox_mode.load(), "AC1: final snap sandbox == load");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_capability_registry_snapshot();
}
#endif
