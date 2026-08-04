// test_mailbox_fiber_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_residual_gc_defer_assert();
extern int run_test_fiber_native_keepalive();
extern int run_test_join_drain_reclaim();
extern int run_test_mailbox_bp_admit();
extern int run_test_mailbox_bp_admit_default();
extern int run_test_chaos_mutate_steal_gc_mailbox();
extern int run_test_fiber_migration_refresh();
extern int run_test_fiber_reclaim_orphan_release();
extern int run_test_is_stealable_snapshot_gate();
extern int run_test_join_drain_timeout();
extern int run_test_mailbox_hold_exit_drain();
extern int run_test_mailbox_hold_starvation_hard();
extern int run_test_mailbox_recv_mutation_boundary();
extern int run_test_mailbox_tenant_principal();
extern int run_test_residual_defer_steal_hard_and();
extern int run_test_residual_force_safepoint();
extern int run_test_steal_complete_gc_defer();
extern int run_test_steal_complete_restamp_txn();
extern int run_test_steal_complete_strong_entry();
extern int run_test_steal_densify_linear_type_hard_and();
extern int run_test_steal_layout_stamp();
extern int run_test_steal_safety_ticket();
extern int run_test_steal_snapshot_hard_invariant();
extern int run_test_steal_snapshot_soft_production_lock();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_mailbox_fiber_batch (24 members) ===");

    std::println("\n──── test_residual_gc_defer_assert ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_residual_gc_defer_assert() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_residual_gc_defer_assert ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_residual_gc_defer_assert ({} checks)", g_passed);
    }

    std::println("\n──── test_fiber_native_keepalive ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_fiber_native_keepalive() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_fiber_native_keepalive ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_fiber_native_keepalive ({} checks)", g_passed);
    }

    std::println("\n──── test_join_drain_reclaim ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_join_drain_reclaim() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_join_drain_reclaim ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_join_drain_reclaim ({} checks)", g_passed);
    }

    std::println("\n──── test_mailbox_bp_admit ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mailbox_bp_admit() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mailbox_bp_admit ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mailbox_bp_admit ({} checks)", g_passed);
    }

    std::println("\n──── test_mailbox_bp_admit_default ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mailbox_bp_admit_default() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mailbox_bp_admit_default ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mailbox_bp_admit_default ({} checks)", g_passed);
    }

    std::println("\n──── test_chaos_mutate_steal_gc_mailbox ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_chaos_mutate_steal_gc_mailbox() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_chaos_mutate_steal_gc_mailbox ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_chaos_mutate_steal_gc_mailbox ({} checks)", g_passed);
    }

    std::println("\n──── test_fiber_migration_refresh ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_fiber_migration_refresh() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_fiber_migration_refresh ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_fiber_migration_refresh ({} checks)", g_passed);
    }

    std::println("\n──── test_fiber_reclaim_orphan_release ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_fiber_reclaim_orphan_release() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_fiber_reclaim_orphan_release ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_fiber_reclaim_orphan_release ({} checks)", g_passed);
    }

    std::println("\n──── test_is_stealable_snapshot_gate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_is_stealable_snapshot_gate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_is_stealable_snapshot_gate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_is_stealable_snapshot_gate ({} checks)", g_passed);
    }

    std::println("\n──── test_join_drain_timeout ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_join_drain_timeout() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_join_drain_timeout ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_join_drain_timeout ({} checks)", g_passed);
    }

    std::println("\n──── test_mailbox_hold_exit_drain ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mailbox_hold_exit_drain() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mailbox_hold_exit_drain ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mailbox_hold_exit_drain ({} checks)", g_passed);
    }

    std::println("\n──── test_mailbox_hold_starvation_hard ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mailbox_hold_starvation_hard() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mailbox_hold_starvation_hard ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mailbox_hold_starvation_hard ({} checks)", g_passed);
    }

    std::println("\n──── test_mailbox_recv_mutation_boundary ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mailbox_recv_mutation_boundary() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mailbox_recv_mutation_boundary ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mailbox_recv_mutation_boundary ({} checks)", g_passed);
    }

    std::println("\n──── test_mailbox_tenant_principal ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mailbox_tenant_principal() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mailbox_tenant_principal ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mailbox_tenant_principal ({} checks)", g_passed);
    }

    std::println("\n──── test_residual_defer_steal_hard_and ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_residual_defer_steal_hard_and() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_residual_defer_steal_hard_and ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_residual_defer_steal_hard_and ({} checks)", g_passed);
    }

    std::println("\n──── test_residual_force_safepoint ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_residual_force_safepoint() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_residual_force_safepoint ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_residual_force_safepoint ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_complete_gc_defer ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_complete_gc_defer() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_steal_complete_gc_defer ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_complete_gc_defer ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_complete_restamp_txn ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_complete_restamp_txn() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_steal_complete_restamp_txn ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_complete_restamp_txn ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_complete_strong_entry ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_complete_strong_entry() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_steal_complete_strong_entry ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_complete_strong_entry ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_densify_linear_type_hard_and ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_densify_linear_type_hard_and() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_steal_densify_linear_type_hard_and ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_densify_linear_type_hard_and ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_layout_stamp ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_layout_stamp() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_steal_layout_stamp ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_layout_stamp ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_safety_ticket ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_safety_ticket() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_steal_safety_ticket ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_safety_ticket ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_snapshot_hard_invariant ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_snapshot_hard_invariant() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_steal_snapshot_hard_invariant ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_snapshot_hard_invariant ({} checks)", g_passed);
    }

    std::println("\n──── test_steal_snapshot_soft_production_lock ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_steal_snapshot_soft_production_lock() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_steal_snapshot_soft_production_lock ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_steal_snapshot_soft_production_lock ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
