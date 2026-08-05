// @category: unit
// @reason: Issue #2427 — CapabilityRegistry::sandbox_mode (and default_tenant)
//          are atomic; concurrent setter + effect-check readers are safe.
//
//   AC1: sandbox_mode is atomic-backed (AtomicEffectSandboxMode)
//   AC2: concurrent setter + load / check_and_record_effect (TSan-friendly)
//   AC3: assignment + enum comparison preserved (caller-compatible)
//   AC4: audit_ring captures sandbox_mode snapshot at record time

#include "test_harness.hpp"

#include "core/capability_model.hh"
#include "core/sandbox.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;

namespace {

using aura::core::capability::check_and_record_effect;
using aura::core::capability::Effect;
using aura::core::capability::EffectAuditEntry;
using aura::core::capability::EffectProvenance;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_registry;
using aura::test::g_failed;
using aura::test::g_passed;

void reset_reg() {
    g_capability_registry().clear_for_test();
}

} // namespace

int run_test_sandbox_mode_atomic() {
    std::println("=== Issue #2427: sandbox_mode atomic (F3+F4 with default_tenant) ===");

    // ── AC1 / AC3 atomic + signature preservation ──────────────────
    {
        std::println("\n--- #2427 AC1 + #2427 AC3: atomic + assignment preserved ---");
        reset_reg();
        auto& reg = g_capability_registry();
        CHECK(reg.sandbox_mode.load() == EffectSandboxMode::Off, "AC1: default Off");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
        CHECK(reg.sandbox_mode == EffectSandboxMode::Strict, "AC3: assign+compare Strict");
        reg.sandbox_mode.store(EffectSandboxMode::Restricted, std::memory_order_release);
        CHECK(reg.sandbox_mode.load(std::memory_order_acquire) == EffectSandboxMode::Restricted,
              "AC1: store/load acquire");
        // F4 default_tenant same pattern
        reg.default_tenant = 99;
        CHECK(reg.default_tenant.load() == 99, "AC1/F4: default_tenant atomic assign");
        CHECK(std::atomic<std::uint8_t>::is_always_lock_free, "AC1: uint8 lock-free");
    }

    // ── AC4 audit stamps sandbox_mode ──────────────────────────────
    {
        std::println("\n--- #2427 AC4: audit captures sandbox_mode ---");
        reset_reg();
        auto& reg = g_capability_registry();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
        EffectProvenance prov{};
        prov.epoch = 1;
        prov.mutation_id = 1;
        reg.record_audit(Effect::Mutate, Effect::None, /*tenant=*/1, prov, /*denied=*/true,
                         "ac4-strict-deny");
        EffectAuditEntry e{};
        CHECK(reg.try_load_latest_audit(e), "AC4: latest audit");
        CHECK(e.sandbox_mode == EffectSandboxMode::Strict, "AC4: audit.sandbox_mode Strict");
        CHECK(e.denied, "AC4: denied flag");

        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        reg.record_audit(Effect::Read, Effect::Read, 1, prov, false, "ac4-off-allow");
        CHECK(reg.try_load_latest_audit(e), "AC4: second audit");
        CHECK(e.sandbox_mode == EffectSandboxMode::Off, "AC4: audit.sandbox_mode Off");
        CHECK(!e.denied, "AC4: allow");
    }

    // ── AC2 concurrent setter + reader ─────────────────────────────
    {
        std::println("\n--- #2427 AC2: concurrent sandbox_mode set + effect check ---");
        reset_reg();
        auto& reg = g_capability_registry();
        // Grant Mutate so Strict still allows when policy is Strict.
        EffectProvenance gprov{};
        gprov.epoch = 1;
        gprov.mutation_id = 1;
        reg.grant(/*tenant=*/1, "mutate-2427", Effect::Mutate, gprov);

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> sets{0};
        std::atomic<std::uint64_t> loads{0};
        std::atomic<std::uint64_t> checks{0};
        std::atomic<std::uint64_t> err{0};
        std::atomic<std::uint64_t> illegal{0};

        std::vector<std::thread> threads;
        // 2 writers flip Off / Restricted / Strict + default_tenant
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t]() {
                std::uint64_t i = static_cast<std::uint64_t>(t);
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        // Issue #2657: route through the process-wide authority.
                        // Numeric values of EffectSandboxMode and SandboxMode
                        // match (0/1/2), so cast + set_mode is the single writer.
                        const auto m = static_cast<EffectSandboxMode>(i % 3);
                        aura::core::sandbox::set_mode(static_cast<aura::core::sandbox::SandboxMode>(
                            static_cast<std::uint8_t>(m)));
                        reg.default_tenant = i % 7;
                        sets.fetch_add(1, std::memory_order_relaxed);
                        ++i;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // 2 pure loaders
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&]() {
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        const auto m = reg.sandbox_mode.load(std::memory_order_acquire);
                        const auto u = static_cast<std::uint8_t>(m);
                        if (u > 2)
                            illegal.fetch_add(1, std::memory_order_relaxed);
                        (void)reg.default_tenant.load(std::memory_order_acquire);
                        loads.fetch_add(1, std::memory_order_relaxed);
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // 2 effect-check readers (hot path)
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&]() {
                EffectProvenance prov{};
                prov.epoch = 1;
                prov.mutation_id = 1;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        (void)check_and_record_effect(Effect::Mutate, Effect::Mutate, prov,
                                                      /*tenant=*/1, "ac2-hot",
                                                      /*wildcard_ok=*/false,
                                                      /*sandbox_active=*/true);
                        checks.fetch_add(1, std::memory_order_relaxed);
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

        std::println("  sets={} loads={} checks={} illegal={} err={}", sets.load(), loads.load(),
                     checks.load(), illegal.load(), err.load());
        CHECK(sets.load() > 0, "AC2: concurrent setters progressed");
        CHECK(loads.load() > 0, "AC2: concurrent loaders progressed");
        CHECK(checks.load() > 0, "AC2: concurrent effect checks progressed");
        CHECK(illegal.load() == 0, "AC2: no illegal mode values (no tear)");
        CHECK(err.load() == 0, "AC2: no exceptions");

        // Final mode is a legal enum; audit carries a stamped mode.
        const auto final_m = reg.sandbox_mode.load();
        CHECK(static_cast<std::uint8_t>(final_m) <= 2, "AC2: final mode legal");
        EffectAuditEntry latest{};
        if (reg.try_load_latest_audit(latest)) {
            CHECK(static_cast<std::uint8_t>(latest.sandbox_mode) <= 2,
                  "AC4: latest audit mode legal");
        }
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_sandbox_mode_atomic();
}
#endif
