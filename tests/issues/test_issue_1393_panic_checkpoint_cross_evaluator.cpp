// @category: integration
// @reason: verifies PanicCheckpointGuard cross-evaluator
//          discriminator check (Issue #1393). Constructs a
//          PanicCheckpointHost with mismatched expected_evaluator_id
//          and verifies Guard dtor bumps restores_discriminator_failed
//          + skips restore (NO UB).
//
// test_issue_1393_panic_checkpoint_cross_evaluator.cpp — Issue #1393:
// PanicCheckpoint × fiber cross-evaluator contract + test.
//
// Background: PanicCheckpointHost's void* ctx becomes stale when
// a Guard constructed on Evaluator A is restored after a migration
// to Evaluator B (aot:reload / persist:load / fiber with cross-
// evaluator body). Restore on the wrong Evaluator would silently
// restore wrong state (UB for the user).
//
// Fix (Issue #1393):
// - Added `void* expected_evaluator_id` discriminator field to
//   PanicCheckpointHost.
// - PanicCheckpointGuard dtor: if expected_evaluator_id != ctx,
//   bump `restores_discriminator_failed` stats counter + skip
//   restore (no UB).
// - panic_checkpoint_host() in evaluator.ixx sets
//   `expected_evaluator_id = &ev` so normal single-Evaluator
//   usage still works (ctx == expected).
//
// Tests:
//   AC1: discriminator mismatch bumps stats counter (no UB)
//   AC2: matching discriminator (single Evaluator) does NOT bump
//        mismatch counter (normal flow preserved)
//   AC3: expected_evaluator_id field is exposed in PanicCheckpointHost

#include "test_harness.hpp"

import std;

import aura.core;
import aura.core.panic_checkpoint_raii;

namespace aura_issue_1393_detail {

// AC3: expected_evaluator_id field is exposed in PanicCheckpointHost
bool test_ac3_discriminator_field_present() {
    std::println("\n--- AC3: expected_evaluator_id field exposed ---");
    aura::core::panic_cp::PanicCheckpointHost h{};
    CHECK(true, "AC3: PanicCheckpointHost struct compiles with "
                "expected_evaluator_id field");
    // Verify the field exists by reading its default value.
    auto* ctx = h.ctx;
    auto* expected = h.expected_evaluator_id;
    std::println("  AC3: h.ctx={}, h.expected_evaluator_id={}", (void*)ctx, (void*)expected);
    CHECK(ctx == nullptr, "AC3: ctx defaults to nullptr");
    CHECK(expected == nullptr, "AC3: expected_evaluator_id defaults to nullptr");
    return true;
}

// AC1: discriminator mismatch bumps stats counter
bool test_ac1_mismatch_bumps_counter() {
    std::println("\n--- AC1: mismatch bumps stats counter ---");
    // Reset stats to clean state
    aura::core::panic_cp::reset_panic_checkpoint_raii_stats();
    const auto before =
        aura::core::panic_cp::g_panic_checkpoint_raii_stats.restores_discriminator_failed;

    // Build a Host with mismatched discriminator (simulates
    // cross-evaluator scenario): ctx != expected_evaluator_id.
    // Both save_fn and restore_fn are no-ops (we don't actually
    // want to call save/restore — the test verifies the
    // discriminator check fires BEFORE restore).
    int dummy_a = 0;
    int dummy_b = 0;
    aura::core::panic_cp::PanicCheckpointHost host{
        &dummy_a,                                    // ctx (different from expected)
        &dummy_b,                                    // expected_evaluator_id
        [](void*) noexcept -> bool { return true; }, // save (no-op)
        [](void*) noexcept -> bool { return true; }, // restore (no-op)
    };

    {
        // Construct Guard — dtor will fire on scope exit
        aura::core::panic_cp::PanicCheckpointGuard guard(host);
        std::println("  AC1: Guard constructed (mismatch host)");
    } // <-- guard dtor fires here; should bump restores_discriminator_failed

    const auto after =
        aura::core::panic_cp::g_panic_checkpoint_raii_stats.restores_discriminator_failed;
    std::println("  AC1: restores_discriminator_failed: {} -> {}", before, after);
    CHECK(after == before + 1, "AC1: mismatch bumps restores_discriminator_failed by 1");
    return true;
}

// AC2: matching discriminator (single Evaluator) does NOT bump
// mismatch counter (normal flow preserved)
bool test_ac2_matching_no_mismatch_bump() {
    std::println("\n--- AC2: matching discriminator preserves normal flow ---");
    aura::core::panic_cp::reset_panic_checkpoint_raii_stats();
    const auto before =
        aura::core::panic_cp::g_panic_checkpoint_raii_stats.restores_discriminator_failed;
    const auto restores_ok_before = aura::core::panic_cp::g_panic_checkpoint_raii_stats.restores_ok;

    // Build a Host with MATCHING discriminator (normal case):
    // ctx == expected_evaluator_id.
    int dummy = 0;
    aura::core::panic_cp::PanicCheckpointHost host{
        &dummy,                                      // ctx
        &dummy,                                      // expected_evaluator_id (same)
        [](void*) noexcept -> bool { return true; }, // save (succeeds)
        [](void*) noexcept -> bool { return true; }, // restore (succeeds)
    };

    {
        aura::core::panic_cp::PanicCheckpointGuard guard(host);
        std::println("  AC2: Guard constructed (matching host)");
    } // <-- dtor; restore should fire (not skipped)

    const auto after_mismatch =
        aura::core::panic_cp::g_panic_checkpoint_raii_stats.restores_discriminator_failed;
    const auto after_ok = aura::core::panic_cp::g_panic_checkpoint_raii_stats.restores_ok;
    std::println("  AC2: restores_discriminator_failed: {} -> {} (should be unchanged)", before,
                 after_mismatch);
    std::println("  AC2: restores_ok: {} -> {} (should be +1)", restores_ok_before, after_ok);
    CHECK(after_mismatch == before, "AC2: matching discriminator does NOT bump mismatch counter");
    CHECK(after_ok == restores_ok_before + 1,
          "AC2: matching discriminator triggers restore (restores_ok +1)");
    return true;
}

} // namespace aura_issue_1393_detail

int main() {
    using namespace aura_issue_1393_detail;
    bool ok = true;
    ok &= test_ac3_discriminator_field_present();
    ok &= test_ac1_mismatch_bumps_counter();
    ok &= test_ac2_matching_no_mismatch_bump();

    int failed = aura::test::g_failed;
    std::println("\n=== Issue #1393 panic checkpoint cross-evaluator: {} ({} failures) ===",
                 (ok && failed == 0) ? "PASS" : "FAIL", failed);
    return (ok && failed == 0) ? 0 : 1;
}