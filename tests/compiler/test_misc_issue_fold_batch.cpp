// test_misc_issue_fold_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include "compiler/coercion_provenance_policy.hh"
#include "compiler/pipeline_policy.hh"
#include "compiler/typed_mutation_audit.h"

#include <print>

import std;

// Isolation/security members leave production face + tree-walker
// Forbidden; later set-code/eval members then fail. Reset between
// members (same as test_linear_misc_batch).
static void reset_member_face() {
    aura::compiler::reset_tree_walker_fallback_policy_for_test();
    aura::compiler::typed_audit::reset_for_test();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::compiler::reset_coercion_provenance_miss_policy_for_test();
}

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

    const auto run = [&](const char* name, int (*fn)()) {
        std::println("\n──── {} ────", name);
        reset_member_face();
        g_passed = 0;
        g_failed = 0;
        if (fn() != 0 || g_failed != 0) {
            ++members_failed;
            std::println("FAIL member {} ({}/{})", name, g_passed, g_failed);
        } else {
            ++members_passed;
            std::println("OK member {} ({} checks)", name, g_passed);
        }
    };

    run("test_aether_denseness_residual", run_test_aether_denseness_residual);
    run("test_audit_wal_force_multi_tenant", run_test_audit_wal_force_multi_tenant);
    run("test_aura_sandbox_env", run_test_aura_sandbox_env);
    run("test_bugfix", run_test_bugfix);
    run("test_commercial_tenant_profile", run_test_commercial_tenant_profile);
    run("test_compact_policy", run_test_compact_policy);
    run("test_delta_truncate_goal_priority", run_test_delta_truncate_goal_priority);
    run("test_dual_path_desync_hard_fail", run_test_dual_path_desync_hard_fail);
    run("test_escape_move_elision_gate", run_test_escape_move_elision_gate);
    run("test_incremental_soundness_oracle", run_test_incremental_soundness_oracle);
    run("test_isolation_audit_mid", run_test_isolation_audit_mid);
    run("test_mutate_capability_force", run_test_mutate_capability_force);
    run("test_orch_scope_child", run_test_orch_scope_child);
    run("test_post_compact_lifecycle", run_test_post_compact_lifecycle);
    run("test_reverify_expand", run_test_reverify_expand);
    run("test_rollback_by_marker", run_test_rollback_by_marker);
    run("test_selfevo_bugfix", run_test_selfevo_bugfix);
    run("test_shape_profiler_concurrency", run_test_shape_profiler_concurrency);
    run("test_symbol_eq", run_test_symbol_eq);
    run("test_truncate_commit_gate", run_test_truncate_commit_gate);
    run("test_try_catch_bind", run_test_try_catch_bind);
    run("test_while_define_oneshot", run_test_while_define_oneshot);
    run("test_fixup_deltas", run_test_fixup_deltas);
    run("test_reset_slot_parent_edges", run_test_reset_slot_parent_edges);
    run("test_restamp_sla_observability", run_test_restamp_sla_observability);
    run("test_subtree_dirty_bounds", run_test_subtree_dirty_bounds);
    run("test_transaction_guard", run_test_transaction_guard);

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
