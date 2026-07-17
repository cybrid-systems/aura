// @category: integration
// @reason: uses AOT bridge C-linkage API + Evaluator atomic bump helpers +
//          per-closure provenance accessors + C-side bridge_epoch wire for
//          the #1485 P0 safety surface (refine #1475 apply_closure epoch
//          check, follow-on from #1508 dual-provenance). No CompilerService,
//          no LLVM JIT — minimal surface so the test links fast even with
//          the system 5-min build timeout.
//
// test_issue_1485.cpp — Verify Issue #1485 acceptance criteria:
//
//   1. stale-closure defense in depth (refine #1475 apply_closure epoch check)
//   2. per-closure provenance accessors for JIT emit-side freshness probe
//   3. bridge_epoch wire between CompilerService and the C runtime
//      (lib/runtime.c aura_closure_call 2-check)
//
// Background: #1485 ships the C1/C2/C2-wire audit plan as 4 commits on main:
//
//   - C1 (3d7f861f) — 2 new atomics on CompilerMetrics:
//                       * stale_closure_prevented — lifetime count of stale
//                         closures detected at apply_closure entry
//                       * closure_epoch_mismatch_fallback — lifetime count of
//                         safe-fallback paths taken after a bridge_epoch /
//                         defuse_version_ mismatch (epoch_stale excludes
//                         linear-only stale so linear_post_mutate_enforce
//                         fallbacks don't bleed into the metric)
//                     + 4 Evaluator accessors / bump helpers wired to the
//                       CompilerMetrics atomics via compiler_metrics_ ptr
//                     + wire in closure_needs_safe_fallback helper (epoch_stale
//                       local tracks bridge_epoch / env_frame mismatch only)
//                     + inline race-window bump in apply_closure dual-path for
//                       flat*/pool* dangling case
//   - C2 (632add22) — per-closure provenance accessors
//                       * aura_get_closure_bridge_epoch(int64_t closure_id)
//                       * aura_get_closure_defuse_version(int64_t closure_id)
//                     Reads under shared_lock on g_closure_table_mtx so
//                     concurrent alloc/free (which resize under unique_lock)
//                     don't race. Out-of-range returns 0.
//                     Weak stubs in aura_jit_bridge_stub.cpp so test binaries
//                     that don't link the runtime still compile.
//   - C2-wire fix-up (7b03974f) — service.ixx::bump_bridge_epoch now also
//                                calls aura_set_current_bridge_epoch(...) so
//                                lib/runtime.c's aura_closure_call 2-check
//                                (bridge_epoch mismatch + defuse_version_
//                                mismatch → return 0) sees the fresh value
//                                instead of the static-init default 0.
//                                Acquire/release pairing mirrors #1476.
//
// Test strategy: 10 ACs, one per public surface + integration. All pure
// C-linkage + Evaluator helpers, no CompilerService, no LLVM JIT — matches
// the link-budget pattern from #1480 (test_incremental_aot_closure_deps.cpp).
//
// Per #1478 / #1480 / #1481 / #1482 precedent, this file is added to
// tests/test-binding-allowlist.txt in case the link hits the system
// 5-min build timeout (per invariant #29). Verification of the link itself
// is deferred to follow-up #1538 batch.
//
// 10 ACs covering the post-C1/C2/C2-wire invariants:
//
//   AC1:  fresh Evaluator + fresh CompilerMetrics →
//         get_stale_closure_prevented() == 0 (initial state).
//   AC2:  bump_stale_closure_prevented() bumps the atomic by exactly 1
//         (verified via get_stale_closure_prevented).
//   AC3:  bump_closure_epoch_mismatch_fallback() bumps the atomic by 1
//         (verified via get_closure_epoch_mismatch_fallback).
//   AC4:  3 consecutive bump_stale_closure_prevented → counter == 3
//         (verifies cumulative behavior).
//   AC5:  Evaluator accessors are no-op when compiler_metrics_ is unset
//         (returns 0 instead of crashing through nullptr).
//   AC6:  aura_get_closure_bridge_epoch(-1) returns 0 (negative out of range).
//   AC7:  aura_get_closure_bridge_epoch(99999) returns 0 (way beyond table).
//   AC8:  aura_get_closure_defuse_version(-1) returns 0.
//   AC9:  aura_get_closure_defuse_version(99999) returns 0.
//   AC10: C-side bridge wire — aura_set_current_bridge_epoch(N) ↔
//         aura_get_current_bridge_epoch() roundtrip. This is the exact
//         mechanism service.ixx::bump_bridge_epoch uses after the C2-wire
//         fix-up; without that wire the C-side g_current_bridge_epoch stays
//         at the static-init default 0 and lib/runtime.c's aura_closure_call
//         2-check passes vacuously.

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <format>
#include <print>
#include <string>

import aura.compiler.evaluator;

namespace {
#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++::aura::test::g_passed;                                                              \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++::aura::test::g_failed;                                                              \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)
} // namespace

int aura_issue_1485_run() {
    using namespace ::aura::compiler;

    std::println(
        "\n=== test_issue_1485 — stale-closure metrics + per-closure provenance + bridge wire ===");

    // ── AC1: fresh metrics — initial state is 0 ────────────────────────
    {
        Evaluator ev;
        CompilerMetrics metrics;
        ev.set_compiler_metrics(static_cast<void*>(&metrics));

        const auto initial_scp = ev.get_stale_closure_prevented();
        const auto initial_cem = ev.get_closure_epoch_mismatch_fallback();
        CHECK(initial_scp == 0,
              std::format("AC1: fresh metrics have stale_closure_prevented == 0 (got {})",
                          initial_scp));
        CHECK(initial_cem == 0,
              std::format("AC1: fresh metrics have closure_epoch_mismatch_fallback == 0 (got {})",
                          initial_cem));
    }

    // ── AC2: bump_stale_closure_prevented bumps atomic by 1 ─────────────
    {
        Evaluator ev;
        CompilerMetrics metrics;
        ev.set_compiler_metrics(static_cast<void*>(&metrics));

        const auto before = ev.get_stale_closure_prevented();
        ev.bump_stale_closure_prevented();
        const auto after = ev.get_stale_closure_prevented();
        CHECK(after == before + 1,
              std::format("AC2: bump_stale_closure_prevented increments atomic by 1 "
                          "(was {}, now {})",
                          before, after));
    }

    // ── AC3: bump_closure_epoch_mismatch_fallback bumps atomic by 1 ─────
    {
        Evaluator ev;
        CompilerMetrics metrics;
        ev.set_compiler_metrics(static_cast<void*>(&metrics));

        const auto before = ev.get_closure_epoch_mismatch_fallback();
        ev.bump_closure_epoch_mismatch_fallback();
        const auto after = ev.get_closure_epoch_mismatch_fallback();
        CHECK(after == before + 1,
              std::format("AC3: bump_closure_epoch_mismatch_fallback increments atomic by 1 "
                          "(was {}, now {})",
                          before, after));
    }

    // ── AC4: 3 consecutive bumps → counter == 3 ───────────────────────
    {
        Evaluator ev;
        CompilerMetrics metrics;
        ev.set_compiler_metrics(static_cast<void*>(&metrics));

        const auto before = ev.get_stale_closure_prevented();
        ev.bump_stale_closure_prevented();
        ev.bump_stale_closure_prevented();
        ev.bump_stale_closure_prevented();
        const auto after = ev.get_stale_closure_prevented();
        CHECK(
            after == before + 3,
            std::format("AC4: 3 consecutive bumps increment by 3 (was {}, now {})", before, after));
    }

    // ── AC5: accessors are no-op when compiler_metrics_ is unset ───────
    {
        Evaluator ev_no_metrics; // no set_compiler_metrics call

        CHECK(ev_no_metrics.get_stale_closure_prevented() == 0,
              "AC5: get_stale_closure_prevented returns 0 when compiler_metrics_ unset");
        CHECK(ev_no_metrics.get_closure_epoch_mismatch_fallback() == 0,
              "AC5: get_closure_epoch_mismatch_fallback returns 0 when compiler_metrics_ unset");

        // bump_* are noexcept no-ops on unset pointer — verify they don't crash
        // through nullptr and the getters still return 0 after.
        ev_no_metrics.bump_stale_closure_prevented();
        ev_no_metrics.bump_closure_epoch_mismatch_fallback();
        CHECK(ev_no_metrics.get_stale_closure_prevented() == 0,
              "AC5: bump_* on unset metrics doesn't write through nullptr (still 0)");
        CHECK(ev_no_metrics.get_closure_epoch_mismatch_fallback() == 0,
              "AC5: bump_* on unset metrics doesn't write through nullptr (still 0)");
    }

    // ── AC6: aura_get_closure_bridge_epoch(-1) → 0 (negative out of range)
    {
        const auto v = aura_get_closure_bridge_epoch(-1);
        CHECK(v == 0, std::format("AC6: aura_get_closure_bridge_epoch(-1) returns 0 (got {})", v));
    }

    // ── AC7: aura_get_closure_bridge_epoch(big) → 0 (beyond table) ─────
    {
        const auto v = aura_get_closure_bridge_epoch(99999);
        CHECK(v == 0,
              std::format("AC7: aura_get_closure_bridge_epoch(99999) returns 0 (got {})", v));
    }

    // ── AC8: aura_get_closure_defuse_version(-1) → 0 ──────────────────
    {
        const auto v = aura_get_closure_defuse_version(-1);
        CHECK(v == 0,
              std::format("AC8: aura_get_closure_defuse_version(-1) returns 0 (got {})", v));
    }

    // ── AC9: aura_get_closure_defuse_version(big) → 0 ─────────────────
    {
        const auto v = aura_get_closure_defuse_version(99999);
        CHECK(v == 0,
              std::format("AC9: aura_get_closure_defuse_version(99999) returns 0 (got {})", v));
    }

    // ── AC10: C-side bridge wire — the C2-wire fix-up mechanism ───────
    {
        // Roundtrip 1: set 42, expect 42
        aura_set_current_bridge_epoch(42);
        const auto read1 = aura_get_current_bridge_epoch();
        CHECK(read1 == 42, std::format("AC10: aura_set_current_bridge_epoch(42) → "
                                       "aura_get_current_bridge_epoch() returns 42 (got {})",
                                       read1));

        // Roundtrip 2: overwrite with 7, expect 7 (verifies last-write-wins,
        // not a one-shot init).
        aura_set_current_bridge_epoch(7);
        const auto read2 = aura_get_current_bridge_epoch();
        CHECK(read2 == 7,
              std::format("AC10: aura_set_current_bridge_epoch(7) → "
                          "aura_get_current_bridge_epoch() returns 7 (got {}) — last-write-wins",
                          read2));

        // Reset to default for downstream tests (don't leak state).
        aura_set_current_bridge_epoch(0);
        const auto read3 = aura_get_current_bridge_epoch();
        CHECK(read3 == 0, std::format("AC10: aura_set_current_bridge_epoch(0) → "
                                      "aura_get_current_bridge_epoch() returns 0 (got {})",
                                      read3));
    }

    std::println("\n=== test_issue_1485 — passed {}, failed {} ===\n", ::aura::test::g_passed,
                 ::aura::test::g_failed);
    return ::aura::test::g_failed == 0 ? 0 : 1;
}

int main() {
    return aura_issue_1485_run();
}
