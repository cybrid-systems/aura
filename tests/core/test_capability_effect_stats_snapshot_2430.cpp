// @category: unit
// @reason: Issue #2430 — snapshot_capability_effect_stats double-check
//          (#1840 pattern) for consistent multi-field view.
//
//   AC1: snapshot consistent under concurrent metric writers
//   AC2: concurrent grant/check/revoke + snapshot (TSan-friendly)
//   AC3: explicit acquire memory orders (source-cite via linter)
//   AC4: existing field names / consumers unchanged

#include "test_harness.hpp"

#include "core/capability_model.hh"

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
using aura::core::capability::EffectProvenance;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::capability::snapshot_capability_effect_stats;
using aura::test::g_failed;
using aura::test::g_passed;

} // namespace

int main() {
    std::println("=== Issue #2430: snapshot_capability_effect_stats double-check ===");

    // ── AC4 single-thread field coherence ──────────────────────────
    {
        std::println("\n--- #2430 AC3 + #2430 AC4: non-racing field names / values ---");
        reset_capability_effects_for_test();
        auto& reg = g_capability_registry();
        reg.sandbox_mode = EffectSandboxMode::Strict;
        EffectProvenance prov{};
        prov.epoch = 1;
        prov.mutation_id = 1;
        reg.grant(/*tenant=*/1, "mut-2430", Effect::Mutate, prov);

        CHECK(
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 1, "ac4-ok", false, true),
            "AC4: allowed under grant");
        CHECK(!check_and_record_effect(Effect::Ffi, Effect::None, prov, 1, "ac4-deny", false, true),
              "AC4: denied without Ffi grant");

        const auto s = snapshot_capability_effect_stats();
        CHECK(s.checks >= 2, "AC4: checks counted");
        CHECK(s.enforced >= 1, "AC4: enforced counted");
        CHECK(s.denied >= 1, "AC4: denied counted");
        CHECK(s.grants >= 1, "AC4: grants counted");
        CHECK(s.audits >= 2, "AC4: audits counted");
        CHECK(s.sandbox_mode == static_cast<int>(EffectSandboxMode::Strict), "AC4: sandbox mode");
        CHECK(s.phase > 0, "AC4: phase present");
        CHECK(s.issue > 0, "AC4: issue present");
        // Internal consistency: checks should cover enforced+denied at least
        // (other paths may bump checks too).
        CHECK(s.checks >= s.enforced, "AC4: checks >= enforced");
        CHECK(s.checks >= s.denied, "AC4: checks >= denied");
    }

    // ── AC1/AC2 concurrent writers + snapshot ──────────────────────
    {
        std::println("\n--- #2430 AC1 + #2430 AC2: concurrent check + snapshot ---");
        reset_capability_effects_for_test();
        auto& reg = g_capability_registry();
        reg.sandbox_mode = EffectSandboxMode::Restricted;
        EffectProvenance gprov{};
        gprov.epoch = 1;
        gprov.mutation_id = 1;
        reg.grant(1, "r-2430", Effect::Read, gprov);

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> writes{0};
        std::atomic<std::uint64_t> snaps{0};
        std::atomic<std::uint64_t> err{0};
        std::atomic<std::uint64_t> torn{0};

        std::vector<std::thread> threads;
        // 2 check writers (allow + deny mix)
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t]() {
                EffectProvenance prov{};
                prov.epoch = 1;
                prov.mutation_id = 1;
                std::uint64_t i = static_cast<std::uint64_t>(t);
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        if ((i & 1u) == 0)
                            (void)check_and_record_effect(Effect::Read, Effect::Read, prov, 1,
                                                          "ac2-ok", false, true);
                        else
                            (void)check_and_record_effect(Effect::Write, Effect::None, prov, 1,
                                                          "ac2-deny", false, true);
                        writes.fetch_add(1, std::memory_order_relaxed);
                        ++i;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // 1 grant/revoke writer
        threads.emplace_back([&]() {
            std::uint64_t i = 0;
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    EffectProvenance prov{};
                    prov.epoch = 1;
                    prov.mutation_id = 1;
                    const auto name = std::format("g{}", i % 4);
                    reg.grant(2, name, Effect::Mutate, prov);
                    if ((i & 1u) != 0)
                        reg.revoke(2, name, 1);
                    writes.fetch_add(1, std::memory_order_relaxed);
                    ++i;
                } catch (...) {
                    err.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        // 4 snapshot readers — validate field legality / light consistency
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&]() {
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        const auto s = snapshot_capability_effect_stats();
                        snaps.fetch_add(1, std::memory_order_relaxed);
                        // Legal sandbox enum range
                        if (s.sandbox_mode < 0 || s.sandbox_mode > 2)
                            torn.fetch_add(1, std::memory_order_relaxed);
                        // hard_fiber is 0/1
                        if (s.hard_fiber_isolation != 0 && s.hard_fiber_isolation != 1)
                            torn.fetch_add(1, std::memory_order_relaxed);
                        // Monotonic-ish: checks should be >= max(enforced, denied)
                        // under our writer mix (each check path bumps checks).
                        // Under extreme tear without double-check, enforced can
                        // race ahead of checks; double-check should keep
                        // checks >= enforced for the hot pair we re-verify.
                        if (s.checks < s.enforced || s.checks < s.denied)
                            torn.fetch_add(1, std::memory_order_relaxed);
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

        const auto final_s = snapshot_capability_effect_stats();
        std::println("  writes={} snaps={} torn={} err={} final checks={} enf={} den={} grants={}",
                     writes.load(), snaps.load(), torn.load(), err.load(), final_s.checks,
                     final_s.enforced, final_s.denied, final_s.grants);
        CHECK(writes.load() > 0, "AC2: concurrent writers progressed");
        CHECK(snaps.load() > 0, "AC2: concurrent snapshots progressed");
        CHECK(err.load() == 0, "AC2: no exceptions");
        CHECK(torn.load() == 0, "AC1: no torn illegal/inconsistent snapshots");
        CHECK(final_s.checks >= final_s.enforced, "AC1: final checks >= enforced");
        CHECK(final_s.checks >= final_s.denied, "AC1: final checks >= denied");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
