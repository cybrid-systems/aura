// test_mailbox_fiber_batch.cpp — thematic multi-TU batch
// Mailbox / fiber / residual / steal / chaos
// Members export run_<name>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.
// Policy: tests/HOMES.md — extend this batch, do not add test_*_<issue>.cpp.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_residual_gc_defer_assert_2211();
extern int run_test_fiber_native_keepalive_2159();
extern int run_test_join_drain_reclaim_2227();
extern int run_test_mailbox_bp_admit_2228();
extern int run_test_mailbox_bp_admit_default_2535();
extern int run_test_chaos_mutate_steal_gc_mailbox_2352();
extern int run_test_fiber_migration_refresh_2194();
extern int run_test_fiber_reclaim_orphan_release_2498();
extern int run_test_is_stealable_snapshot_gate_2549();
extern int run_test_join_drain_timeout_2153();
extern int run_test_mailbox_hold_exit_drain_2511();
extern int run_test_mailbox_hold_starvation_hard_2551();
extern int run_test_mailbox_recv_mutation_boundary_2188();
extern int run_test_mailbox_tenant_principal_2592();
extern int run_test_residual_defer_steal_hard_and_2546();
extern int run_test_residual_force_safepoint_2533();
extern int run_test_steal_complete_gc_defer_2203();
extern int run_test_steal_complete_restamp_txn_2510();
extern int run_test_steal_complete_strong_entry_2377();
extern int run_test_steal_densify_linear_type_hard_and_2609();
extern int run_test_steal_layout_stamp_2351();
extern int run_test_steal_safety_ticket_2518();
extern int run_test_steal_snapshot_hard_invariant_2346();
extern int run_test_steal_snapshot_soft_production_lock_2372();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_mailbox_fiber_batch (24 members) ===");

    std::println("\n──── test_residual_gc_defer_assert_2211 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_residual_gc_defer_assert_2211() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_residual_gc_defer_assert_2211 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_residual_gc_defer_assert_2211 ({} checks)", g_passed);
    }

    std::println("\n──── test_fiber_native_keepalive_2159 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_fiber_native_keepalive_2159() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_fiber_native_keepalive_2159 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_fiber_native_keepalive_2159 ({} checks)", g_passed);
    }

    std::println("\n──── test_join_drain_reclaim_2227 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_join_drain_reclaim_2227() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_join_drain_reclaim_2227 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_join_drain_reclaim_2227 ({} checks)", g_passed);
    }

    std::println("\n──── test_mailbox_bp_admit_2228 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mailbox_bp_admit_2228() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mailbox_bp_admit_2228 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mailbox_bp_admit_2228 ({} checks)", g_passed);
    }

    std::println("\n──── test_mailbox_bp_admit_default_2535 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mailbox_bp_admit_default_2535() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_mailbox_bp_admit_default_2535 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mailbox_bp_admit_default_2535 ({} checks)", g_passed);
    }

    std::println("\n──── test_chaos_mutate_steal_gc_mailbox_2352 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_chaos_mutate_steal_gc_mailbox_2352() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_chaos_mutate_steal_gc_mailbox_2352 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_chaos_mutate_steal_gc_mailbox_2352 ({} checks)", g_passed);
    }

    std::println("\n──── test_fiber_migration_refresh_2194 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_fiber_migration_refresh_2194() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_fiber_migration_refresh_2194 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_fiber_migration_refresh_2194 ({} checks)", g_passed);
    }

    std::println("\n──── test_fiber_reclaim_orphan_release_2498 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_fiber_reclaim_orphan_release_2498() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_fiber_reclaim_orphan_release_2498 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_fiber_reclaim_orphan_release_2498 ({} checks)", g_passed);
    }

    std::println("\n──── test_is_stealable_snapshot_gate_2549 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_is_stealable_snapshot_gate_2549() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_is_stealable_snapshot_gate_2549 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_is_stealable_snapshot_gate_2549 ({} checks)", g_passed);
    }

    std::println("\n──── test_join_drain_timeout_2153 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_join_drain_timeout_2153() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_join_drain_timeout_2153 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_join_drain_timeout_2153 ({} checks)", g_passed);
    }

    std::println("\n──── test_mailbox_hold_exit_drain_2511 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mailbox_hold_exit_drain_2511() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mailbox_hold_exit_drain_2511 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mailbox_hold_exit_drain_2511 ({} checks)", g_passed);
    }

    std::println("\n──── test_mailbox_hold_starvation_hard_2551 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mailbox_hold_starvation_hard_2551() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_mailbox_hold_starvation_hard_2551 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mailbox_hold_starvation_hard_2551 ({} checks)", g_passed);
    }

    std::println("\n──── test_mailbox_recv_mutation_boundary_2188 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mailbox_recv_mutation_boundary_2188() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_mailbox_recv_mutation_boundary_2188 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mailbox_recv_mutation_boundary_2188 ({} checks)", g_passed);
    }

    std::println("\n──── test_mailbox_tenant_principal_2592 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mailbox_tenant_principal_2592() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_mailbox_tenant_principal_2592 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mailbox_tenant_principal_2592 ({} checks)", g_passed);
    }

    std::println("\n──── test_residual_defer_steal_hard_and_2546 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_residual_defer_steal_hard_and_2546() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_residual_defer_steal_hard_and_2546 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_residual_defer_steal_hard_and_2546 ({} checks)", g_passed);
    }

    std::println("\n──── test_residual_force_safepoint_2533 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_residual_force_safepoint_2533() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_residual_force_safepoint_2533 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_residual_force_safepoint_2533 ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_complete_gc_defer_2203 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_complete_gc_defer_2203() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_steal_complete_gc_defer_2203 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_complete_gc_defer_2203 ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_complete_restamp_txn_2510 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_complete_restamp_txn_2510() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_steal_complete_restamp_txn_2510 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_complete_restamp_txn_2510 ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_complete_strong_entry_2377 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_complete_strong_entry_2377() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_steal_complete_strong_entry_2377 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_complete_strong_entry_2377 ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_densify_linear_type_hard_and_2609 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_densify_linear_type_hard_and_2609() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_steal_densify_linear_type_hard_and_2609 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_densify_linear_type_hard_and_2609 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_steal_layout_stamp_2351 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_layout_stamp_2351() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_steal_layout_stamp_2351 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_layout_stamp_2351 ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_safety_ticket_2518 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_safety_ticket_2518() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_steal_safety_ticket_2518 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_safety_ticket_2518 ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_snapshot_hard_invariant_2346 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_snapshot_hard_invariant_2346() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_steal_snapshot_hard_invariant_2346 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_snapshot_hard_invariant_2346 ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_snapshot_soft_production_lock_2372 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_snapshot_soft_production_lock_2372() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_steal_snapshot_soft_production_lock_2372 (checks: {} "
                     "passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_snapshot_soft_production_lock_2372 ({} checks)",
                     g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
