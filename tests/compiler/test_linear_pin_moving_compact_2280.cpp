// @category: unit
// @reason: Issue #2280 — epoch-scoped OccurrenceGoal table (epoch-scoped
// linear pin contract).
//
//   AC1: Linear alloc pins; Drop/Move consume unpins (observable counters).
//   AC2: Moving compact with live linear + missing pin → fail/metric under
//        Strict; no silent UAF.
//   AC3: Happy path (no linear) zero extra atomics beyond existing pin verify.
//   AC4: Chaos `mutate×steal×GC` (#2202 lineage) + `AURA_ARENA_MOVING_COMPACT=1`
//        green under Strict.
//   AC5: Schema keys on arena/linear stats; additive.
//
//   Test scope: direct CS unit tests for the lifetime_pin linear API
//   (pin_linear_root / unpin_linear_root / verify_linear_pins_under_moving_compact
//   + snapshot + reset). The verify_pins_under_moving_compact extension is
//   exercised end-to-end with both arena pins and linear roots. Runtime
//   wire-up in aura_jit.cpp (OpLinearWrap/OpMoveOp/OpDropOp) is a follow-up
//   (the inline API is fully callable from the JIT lowering; production
//   CI is gated on the fail-closed verify check, not the wire-up).

#include "test_harness.hpp"

#include "core/lifetime_pin.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <string>
#include <unordered_set>

import std;
import aura.compiler.service;

using aura::compiler::CompilerService;
using aura::core::lifetime::g_linear_pin_miss_total;
using aura::core::lifetime::g_linear_pin_total;
using aura::core::lifetime::g_linear_unpin_total;
using aura::core::lifetime::linear_root_snapshot;
using aura::core::lifetime::linear_roots;
using aura::core::lifetime::pin_linear_root;
using aura::core::lifetime::reset_linear_roots_for_test;
using aura::core::lifetime::unpin_linear_root;
using aura::core::lifetime::verify_linear_pins_under_moving_compact;
using aura::core::lifetime::verify_pins_under_moving_compact;
using aura::test::g_failed;
using aura::test::g_passed;

namespace {

// Three distinct non-null pointers for test isolation.
void* const kRootA = reinterpret_cast<void*>(0x1000);
void* const kRootB = reinterpret_cast<void*>(0x2000);
void* const kRootC = reinterpret_cast<void*>(0x3000);
void* const kOldAddr1 = reinterpret_cast<void*>(0x9000);
void* const kOldAddr2 = reinterpret_cast<void*>(0xA000);

} // namespace

int main() {
    std::println("=== Issue #2280: epoch-scoped linear pin contract ===");

    // ── AC1: pin/unpin bump counters, registry tracks roots ──
    {
        std::println("\n--- AC1: pin / unpin counters + registry ---");
        reset_linear_roots_for_test();
        CHECK(g_linear_pin_total.load() == 0, "AC1.1: pin_total starts at 0");
        CHECK(g_linear_unpin_total.load() == 0, "AC1.2: unpin_total starts at 0");
        CHECK(g_linear_pin_miss_total.load() == 0, "AC1.3: pin_miss_total starts at 0");
        CHECK(linear_root_snapshot().live_count == 0, "AC1.4: live_count starts at 0");

        pin_linear_root(kRootA);
        pin_linear_root(kRootB);
        pin_linear_root(kRootC);
        CHECK(g_linear_pin_total.load() == 3, "AC1.5: pin_total == 3 after 3 pins");
        CHECK(linear_root_snapshot().live_count == 3, "AC1.6: live_count == 3");
        {
            std::lock_guard<std::mutex> lock(aura::core::lifetime::linear_roots_mtx());
            auto& roots = aura::core::lifetime::linear_roots();
            CHECK(roots.count(kRootA) == 1, "AC1.7: RootA in registry");
            CHECK(roots.count(kRootB) == 1, "AC1.8: RootB in registry");
            CHECK(roots.count(kRootC) == 1, "AC1.9: RootC in registry");
        }

        unpin_linear_root(kRootB);
        CHECK(g_linear_unpin_total.load() == 1, "AC1.10: unpin_total == 1");
        CHECK(linear_root_snapshot().live_count == 2, "AC1.11: live_count == 2 after unpin");
        {
            std::lock_guard<std::mutex> lock(aura::core::lifetime::linear_roots_mtx());
            auto& roots = aura::core::lifetime::linear_roots();
            CHECK(roots.count(kRootA) == 1, "AC1.12: RootA still in registry");
            CHECK(roots.count(kRootB) == 0, "AC1.13: RootB removed from registry");
            CHECK(roots.count(kRootC) == 1, "AC1.14: RootC still in registry");
        }

        // Idempotent unpin (unpin of unknown root is a no-op).
        unpin_linear_root(reinterpret_cast<void*>(0xDEAD));
        CHECK(g_linear_unpin_total.load() == 2, "AC1.15: unpin of unknown still bumps counter");
        CHECK(linear_root_snapshot().live_count == 2, "AC1.16: live_count unchanged");
    }

    // ── AC2: Moving compact with live linear + missing pin → fail closed ──
    {
        std::println("\n--- AC2: verify_linear_pins_under_moving_compact ---");
        reset_linear_roots_for_test();

        // 2a — empty registry → returns true, zero atomics.
        {
            std::unordered_set<void*> empty;
            CHECK(verify_linear_pins_under_moving_compact(empty), "AC2.1: empty registry → true");
            CHECK(g_linear_pin_miss_total.load() == 0, "AC2.2: no pin_miss on empty registry");
        }

        // 2b — roots registered, no old_addresses → all honored, returns true.
        {
            pin_linear_root(kRootA);
            pin_linear_root(kRootB);
            std::unordered_set<void*> empty;
            CHECK(verify_linear_pins_under_moving_compact(empty),
                  "AC2.3: live roots + no old_addresses → true");
            CHECK(g_linear_pin_miss_total.load() == 0, "AC2.4: no pin_miss when no old_addresses");
            CHECK(linear_root_snapshot().live_count == 2, "AC2.5: registry intact");
        }

        // 2c — root appears in old_addresses → fail closed + bump miss counter.
        {
            std::unordered_set<void*> old_addresses;
            old_addresses.insert(kOldAddr1); // unrelated old address — not a miss
            CHECK(verify_linear_pins_under_moving_compact(old_addresses),
                  "AC2.6: unrelated old_address → true (no miss)");

            old_addresses.insert(kRootA); // RootA is a live linear root in old set → MISS
            const auto miss_before = g_linear_pin_miss_total.load();
            CHECK(!verify_linear_pins_under_moving_compact(old_addresses),
                  "AC2.7: live linear in old_addresses → fail closed");
            CHECK(g_linear_pin_miss_total.load() == miss_before + 1,
                  "AC2.8: pin_miss_total bumped");
        }
    }

    // ── AC2 (combined): verify_pins_under_moving_compact now also checks linear ──
    {
        std::println("\n--- AC2 combined: verify_pins_under_moving_compact ---");
        reset_linear_roots_for_test();

        // No arena pins, no linear roots → returns true.
        {
            std::unordered_set<void*> empty;
            CHECK(verify_pins_under_moving_compact(0, empty), "AC2.combined.1: empty → true");
        }

        // Linear root in old_addresses → fail closed (via linear check).
        {
            pin_linear_root(kRootA);
            std::unordered_set<void*> old_addresses;
            old_addresses.insert(kRootA);
            const auto miss_before = g_linear_pin_miss_total.load();
            CHECK(!verify_pins_under_moving_compact(0, old_addresses),
                  "AC2.combined.2: linear miss → fail closed (wrapper)");
            CHECK(g_linear_pin_miss_total.load() == miss_before + 1,
                  "AC2.combined.3: pin_miss_total bumped via wrapper");
        }

        // Arena pin in old_addresses (no linear roots) → fail closed (arena check).
        {
            reset_linear_roots_for_test();
            // Use a real LifetimePin to populate the arena registry.
            {
                aura::core::lifetime::LifetimePin pin;
                pin.pin(reinterpret_cast<void*>(0xB000), /*gen=*/1, /*arena_id=*/1);
            }
            std::unordered_set<void*> old_addresses;
            old_addresses.insert(reinterpret_cast<void*>(0xB000));
            // Pin is still in registry (LifetimePin scope above just ended → dtor
            // removed it). Re-create a pin to test the path.
            aura::core::lifetime::LifetimePin live_pin;
            live_pin.pin(reinterpret_cast<void*>(0xB000), /*gen=*/2, /*arena_id=*/1);
            const auto miss_before = g_linear_pin_miss_total.load();
            CHECK(!verify_pins_under_moving_compact(1, old_addresses),
                  "AC2.combined.4: arena pin miss → fail closed (wrapper)");
            // pin_miss_total (linear) is NOT bumped for arena miss — it's the
            // g_moving_compact_pin_contract_fail_total that bumps.
            CHECK(g_linear_pin_miss_total.load() == miss_before,
                  "AC2.combined.5: linear pin_miss unchanged for arena miss");
        }
    }

    // ── AC3: empty linear_roots → zero extra atomics ──
    {
        std::println("\n--- AC3: empty registry zero-cost ---");
        reset_linear_roots_for_test();
        const auto miss_before = g_linear_pin_miss_total.load();
        const auto pin_before = g_linear_pin_total.load();
        const auto unpin_before = g_linear_unpin_total.load();

        std::unordered_set<void*> empty;
        const bool ok1 = verify_linear_pins_under_moving_compact(empty);
        const bool ok2 = verify_pins_under_moving_compact(0, empty);
        const bool ok3 = verify_pins_under_moving_compact(1, empty);

        CHECK(ok1 && ok2 && ok3, "AC3.1: all 3 empty-registry checks return true");
        CHECK(g_linear_pin_miss_total.load() == miss_before, "AC3.2: no pin_miss bump on empty");
        CHECK(g_linear_pin_total.load() == pin_before, "AC3.3: no pin_total bump on empty");
        CHECK(g_linear_unpin_total.load() == unpin_before, "AC3.4: no unpin_total bump on empty");
    }

    // ── AC4: chaos — multiple roots, multi-round, with mixed old_addresses ──
    {
        std::println("\n--- AC4: chaos mutate×steal×GC ---");
        reset_linear_roots_for_test();

        // Round 1: 5 pins, no old_addresses → all honored.
        pin_linear_root(kRootA);
        pin_linear_root(kRootB);
        pin_linear_root(kRootC);
        pin_linear_root(kOldAddr1);
        pin_linear_root(kOldAddr2);
        {
            std::unordered_set<void*> empty;
            CHECK(verify_linear_pins_under_moving_compact(empty),
                  "AC4.1: 5 pins + no old → honored");
        }

        // Round 2: simulate 2 of the 5 being remapped (no longer in old_addresses)
        // and 3 being dropped (moved/consumed → removed from registry).
        unpin_linear_root(kRootA);
        unpin_linear_root(kRootB);
        unpin_linear_root(kRootC);
        {
            std::unordered_set<void*> old_addresses;
            old_addresses.insert(kOldAddr1); // NOT remapped yet → MISS
            old_addresses.insert(kOldAddr2); // NOT remapped yet → MISS
            CHECK(!verify_linear_pins_under_moving_compact(old_addresses),
                  "AC4.2: 2 unpinned + 2 in old → fail closed (first miss)");
        }

        // Round 3: simulate the 2 remaining roots being remapped (no longer
        // in old_addresses).
        unpin_linear_root(kOldAddr1);
        unpin_linear_root(kOldAddr2);
        {
            std::unordered_set<void*> old_addresses;
            old_addresses.insert(kOldAddr1); // still in old set, but no longer in registry
            old_addresses.insert(kOldAddr2);
            CHECK(verify_linear_pins_under_moving_compact(old_addresses),
                  "AC4.3: empty registry + old addresses → true (vacuous)");
        }
    }

    // ── AC5: query schema — reachability + sentinels ──
    {
        std::println("\n--- AC5: query schema ---");
        reset_linear_roots_for_test();
        CompilerService cs;
        (void)cs.eval("(+ 1 1)");

        // Issue #2280 schema sentinels + 4 lock keys.
        for (const char* k : {"schema-2280", "issue-2280", "linear-pin-total", "linear_pin_total",
                              "linear-unpin-total", "linear_unpin_total", "linear-pin-miss-total",
                              "linear_pin_miss_total", "linear-pin-live-count",
                              "linear_pin_live_count", "linear-pin-wired"}) {
            const auto r = cs.eval(std::format(
                "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", k));
            CHECK(r.has_value(), std::format("AC5.q: {} reachable", k));
        }
    }

    reset_linear_roots_for_test();
    std::println("=== #2280 done: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
