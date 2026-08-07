// test_misc_issue_fold_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_aether_denseness_residual();
extern int run_test_audit_wal_force_multi_tenant();
extern int run_test_aura_sandbox_env();
extern int run_test_bugfix();
extern int run_test_commercial_tenant_profile();
extern int run_test_compact_policy();
extern int run_test_delta_truncate_goal_priority();
extern int run_test_dual_path_desync_hard_fail();
extern int run_test_escape_move_elision_gate();
extern int run_test_incremental_soundness_oracle();
extern int run_test_isolation_audit_mid();
extern int run_test_mutate_capability_force();
extern int run_test_orch_scope_child();
extern int run_test_post_compact_lifecycle();
extern int run_test_reverify_expand();
extern int run_test_rollback_by_marker();
extern int run_test_selfevo_bugfix();
extern int run_test_shape_profiler_concurrency();
extern int run_test_symbol_eq();
extern int run_test_truncate_commit_gate();
extern int run_test_try_catch_bind();
extern int run_test_while_define_oneshot();
extern int run_test_fixup_deltas();
extern int run_test_reset_slot_parent_edges();
extern int run_test_restamp_sla_observability();
extern int run_test_subtree_dirty_bounds();
extern int run_test_transaction_guard();
// Reflect members (test_ir_pod_phase4 / test_opcode_reflect /
// test_reflect_isolation) run as standalone -freflection binaries —
// they cannot live in this CXX_MODULE_STD batch (P2996 + import std).

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_misc_issue_fold_batch (27 members) ===");

    std::println("\n──── test_aether_denseness_residual ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_aether_denseness_residual() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_aether_denseness_residual ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_aether_denseness_residual ({} checks)", g_passed);
    }

    std::println("\n──── test_audit_wal_force_multi_tenant ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_audit_wal_force_multi_tenant() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_audit_wal_force_multi_tenant ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_audit_wal_force_multi_tenant ({} checks)", g_passed);
    }

    std::println("\n──── test_aura_sandbox_env ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_aura_sandbox_env() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_aura_sandbox_env ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_aura_sandbox_env ({} checks)", g_passed);
    }

    std::println("\n──── test_bugfix ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_bugfix() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_bugfix ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_bugfix ({} checks)", g_passed);
    }

    std::println("\n──── test_commercial_tenant_profile ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_commercial_tenant_profile() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_commercial_tenant_profile ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_commercial_tenant_profile ({} checks)", g_passed);
    }

    std::println("\n──── test_compact_policy ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_compact_policy() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_compact_policy ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_compact_policy ({} checks)", g_passed);
    }

    std::println("\n──── test_delta_truncate_goal_priority ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_delta_truncate_goal_priority() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_delta_truncate_goal_priority ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_delta_truncate_goal_priority ({} checks)", g_passed);
    }

    std::println("\n──── test_dual_path_desync_hard_fail ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dual_path_desync_hard_fail() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dual_path_desync_hard_fail ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dual_path_desync_hard_fail ({} checks)", g_passed);
    }

    std::println("\n──── test_escape_move_elision_gate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_escape_move_elision_gate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_escape_move_elision_gate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_escape_move_elision_gate ({} checks)", g_passed);
    }

    std::println("\n──── test_incremental_soundness_oracle ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_incremental_soundness_oracle() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_incremental_soundness_oracle ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_incremental_soundness_oracle ({} checks)", g_passed);
    }

    std::println("\n──── test_isolation_audit_mid ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_isolation_audit_mid() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_isolation_audit_mid ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_isolation_audit_mid ({} checks)", g_passed);
    }

    std::println("\n──── test_mutate_capability_force ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutate_capability_force() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutate_capability_force ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutate_capability_force ({} checks)", g_passed);
    }

    std::println("\n──── test_orch_scope_child ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_orch_scope_child() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_orch_scope_child ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_orch_scope_child ({} checks)", g_passed);
    }

    std::println("\n──── test_post_compact_lifecycle ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_post_compact_lifecycle() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_post_compact_lifecycle ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_post_compact_lifecycle ({} checks)", g_passed);
    }

    std::println("\n──── test_reverify_expand ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reverify_expand() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_reverify_expand ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_reverify_expand ({} checks)", g_passed);
    }

    std::println("\n──── test_rollback_by_marker ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_rollback_by_marker() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_rollback_by_marker ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_rollback_by_marker ({} checks)", g_passed);
    }

    std::println("\n──── test_selfevo_bugfix ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_selfevo_bugfix() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_selfevo_bugfix ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_selfevo_bugfix ({} checks)", g_passed);
    }

    std::println("\n──── test_shape_profiler_concurrency ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_shape_profiler_concurrency() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_shape_profiler_concurrency ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_shape_profiler_concurrency ({} checks)", g_passed);
    }

    std::println("\n──── test_symbol_eq ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_symbol_eq() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_symbol_eq ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_symbol_eq ({} checks)", g_passed);
    }

    std::println("\n──── test_truncate_commit_gate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_truncate_commit_gate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_truncate_commit_gate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_truncate_commit_gate ({} checks)", g_passed);
    }

    std::println("\n──── test_try_catch_bind ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_try_catch_bind() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_try_catch_bind ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_try_catch_bind ({} checks)", g_passed);
    }

    std::println("\n──── test_while_define_oneshot ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_while_define_oneshot() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_while_define_oneshot ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_while_define_oneshot ({} checks)", g_passed);
    }

    std::println("\n──── test_fixup_deltas ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_fixup_deltas() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_fixup_deltas ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_fixup_deltas ({} checks)", g_passed);
    }

    std::println("\n──── test_reset_slot_parent_edges ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reset_slot_parent_edges() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_reset_slot_parent_edges ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_reset_slot_parent_edges ({} checks)", g_passed);
    }

    std::println("\n──── test_restamp_sla_observability ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_restamp_sla_observability() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_restamp_sla_observability ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_restamp_sla_observability ({} checks)", g_passed);
    }

    std::println("\n──── test_subtree_dirty_bounds ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_subtree_dirty_bounds() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_subtree_dirty_bounds ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_subtree_dirty_bounds ({} checks)", g_passed);
    }

    std::println("\n──── test_transaction_guard ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_transaction_guard() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_transaction_guard ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_transaction_guard ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
