// @category: unit
// @reason: Issue #2425 — CapabilityRegistry audit_ring published slots
//          (no torn EffectAuditEntry under concurrent write/read).
//
//   AC1: reader sees fully-written entry or prior complete entry (never torn)
//   AC2: concurrent record_audit + try_load (TSan-friendly)
//   AC3: audit_seq / publish_seq use release write + acquire read
//   AC4: non-racing try_load_latest_audit semantics preserved

#include "test_harness.hpp"

#include "core/capability_model.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;

namespace {

using aura::core::capability::Effect;
using aura::core::capability::EffectAuditEntry;
using aura::core::capability::EffectProvenance;
using aura::core::capability::g_capability_registry;
using aura::test::g_failed;
using aura::test::g_passed;

void reset_reg() {
    g_capability_registry().clear_for_test();
}

} // namespace

int main() {
    std::println("=== Issue #2425: capability audit_ring published slots ===");

    // ── AC4 non-racing load ────────────────────────────────────────
    {
        std::println("\n--- #2425 AC4: non-racing try_load_latest_audit ---");
        reset_reg();
        auto& reg = g_capability_registry();
        EffectAuditEntry empty{};
        CHECK(!reg.try_load_latest_audit(empty), "AC4: empty ring → false");
        CHECK(reg.load_audit_seq() == 0, "AC4: seq starts at 0");

        EffectProvenance prov{};
        prov.epoch = 42;
        prov.fiber_id = 7;
        prov.mutation_id = 99;
        reg.record_audit(Effect::Mutate, Effect::Mutate, /*tenant=*/1, prov, /*denied=*/false,
                         "ac4-op-allow");
        CHECK(reg.load_audit_seq() == 1, "AC4: seq advanced");
        EffectAuditEntry e{};
        CHECK(reg.try_load_latest_audit(e), "AC4: latest loads");
        CHECK(e.seq == 0, "AC4: first entry seq 0");
        CHECK(e.tenant_id == 1, "AC4: tenant");
        CHECK(e.prov.epoch == 42, "AC4: prov.epoch intact");
        CHECK(e.prov.fiber_id == 7, "AC4: fiber intact");
        CHECK(!e.denied, "AC4: denied false");
        CHECK(std::string_view(e.op) == "ac4-op-allow", "AC4: op string intact");

        reg.record_audit(Effect::Ffi, Effect::None, 2, prov, /*denied=*/true, "ac4-deny-ffi");
        CHECK(reg.try_load_latest_audit(e), "AC4: second latest");
        CHECK(e.seq == 1, "AC4: second seq");
        CHECK(e.denied, "AC4: denied true");
        CHECK(e.required == Effect::Ffi, "AC4: required Ffi");
        CHECK(e.actual == Effect::None, "AC4: actual None");
        CHECK(std::string_view(e.op) == "ac4-deny-ffi", "AC4: op deny");

        // Load by exact seq still works for prior entry.
        EffectAuditEntry e0{};
        CHECK(reg.try_load_audit_seq(0, e0), "AC4: load seq 0 still published");
        CHECK(std::string_view(e0.op) == "ac4-op-allow", "AC4: prior op intact");
    }

    // ── AC1/AC3 field integrity under sequential publishes ─────────
    {
        std::println("\n--- #2425 AC1 + #2425 AC3: full field publish integrity ---");
        reset_reg();
        auto& reg = g_capability_registry();
        for (int i = 0; i < 200; ++i) {
            EffectProvenance prov{};
            prov.epoch = static_cast<std::uint64_t>(1000 + i);
            prov.mutation_id = static_cast<std::uint64_t>(i);
            prov.fiber_id = static_cast<std::uint32_t>(i % 16);
            const bool denied = (i % 3) == 0;
            const auto op = std::format("op-{}", i);
            reg.record_audit(Effect::Mutate, denied ? Effect::None : Effect::Mutate,
                             static_cast<std::uint64_t>(i), prov, denied, op);
        }
        // Latest must be complete (wrap past 128).
        EffectAuditEntry latest{};
        CHECK(reg.try_load_latest_audit(latest), "AC1: latest after wrap");
        CHECK(latest.seq == 199, "AC1: latest seq 199");
        CHECK(latest.prov.epoch == 1199, "AC1: epoch matches seq payload");
        CHECK(std::string_view(latest.op) == "op-199", "AC1: op not torn");
        // A still-live older seq (199-127 = 72) if not overwritten wait —
        // after 200 writes, slots hold seq 72..199. seq 0 is gone.
        EffectAuditEntry gone{};
        CHECK(!reg.try_load_audit_seq(0, gone), "AC1: wrapped-out seq 0 not loadable");
        EffectAuditEntry live{};
        CHECK(reg.try_load_audit_seq(150, live), "AC1: mid ring seq loadable");
        CHECK(live.seq == 150, "AC1: mid seq field matches");
        CHECK(live.prov.epoch == 1150, "AC1: mid epoch matches (no tear)");
        CHECK(std::string_view(live.op) == "op-150", "AC1: mid op intact");
    }

    // ── AC2 concurrent writers + readers ───────────────────────────
    {
        std::println("\n--- #2425 AC2: concurrent record_audit + try_load ---");
        reset_reg();
        auto& reg = g_capability_registry();

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> writes{0};
        std::atomic<std::uint64_t> loads{0};
        std::atomic<std::uint64_t> ok_loads{0};
        std::atomic<std::uint64_t> torn{0};
        std::atomic<std::uint64_t> err{0};

        std::vector<std::thread> threads;
        // 2 writers
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t]() {
                std::uint64_t i = static_cast<std::uint64_t>(t);
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        EffectProvenance prov{};
                        prov.epoch = 5000 + i;
                        prov.mutation_id = i;
                        prov.fiber_id = static_cast<std::uint32_t>(i & 0xffu);
                        const bool denied = (i & 1u) != 0;
                        const auto op = std::format("cw-{}-{}", t, i);
                        reg.record_audit(Effect::Write, denied ? Effect::None : Effect::Write,
                                         /*tenant=*/10 + (i % 5), prov, denied, op);
                        writes.fetch_add(1, std::memory_order_relaxed);
                        i += 2;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // 4 readers
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&]() {
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        EffectAuditEntry e{};
                        loads.fetch_add(1, std::memory_order_relaxed);
                        if (reg.try_load_latest_audit(e)) {
                            ok_loads.fetch_add(1, std::memory_order_relaxed);
                            // Integrity: if denied, actual should be None for our writers;
                            // op must be NUL-terminated and match seq embedding loosely.
                            const auto op_len = std::strlen(e.op);
                            if (op_len >= sizeof(e.op))
                                torn.fetch_add(1, std::memory_order_relaxed);
                            // prov.epoch was set to 5000+mutation_id style
                            if (e.prov.mutation_id != 0 && e.prov.epoch != 0 && e.prov.epoch < 5000)
                                torn.fetch_add(1, std::memory_order_relaxed);
                            if (e.denied && e.actual != Effect::None)
                                torn.fetch_add(1, std::memory_order_relaxed);
                            if (!e.denied && e.actual != Effect::Write && e.actual != Effect::None)
                                torn.fetch_add(1, std::memory_order_relaxed);
                        }
                        // Also probe a recent seq if any.
                        const auto seq = reg.load_audit_seq();
                        if (seq > 0) {
                            EffectAuditEntry e2{};
                            (void)reg.try_load_audit_seq(seq - 1, e2);
                        }
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

        std::println("  writes={} loads={} ok_loads={} torn={} err={} seq={}", writes.load(),
                     loads.load(), ok_loads.load(), torn.load(), err.load(), reg.load_audit_seq());
        CHECK(writes.load() > 0, "AC2: concurrent writers progressed");
        CHECK(loads.load() > 0, "AC2: concurrent readers progressed");
        CHECK(ok_loads.load() > 0, "AC2: successful loads observed");
        CHECK(torn.load() == 0, "AC1/AC2: no torn field mixes observed");
        CHECK(err.load() == 0, "AC2: no exceptions");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
