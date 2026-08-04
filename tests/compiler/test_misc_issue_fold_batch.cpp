// test_misc_issue_fold_batch.cpp — thematic multi-TU batch
// Leftover issue-suffixed tests (W_other residual after Stream A splits)
// Stream A4 of tests/CONSOLIDATION_PLAN.md — carved from misc_issue_fold.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_adaptive_cascade_depth_partial_thr_2209();
extern int run_test_adaptive_reverify_limit_2146();
extern int run_test_aether_denseness_residual_2578();
extern int run_test_audit_wal_force_multi_tenant_2150();
extern int run_test_aura_sandbox_env_2076();
extern int run_test_bugfix_968();
extern int run_test_cascade_incremental_pass_suite_2044();
extern int run_test_cascade_skip_metrics_2106();
extern int run_test_channel_rendezvous_2483();
extern int run_test_command_line_cap_io_read_2478();
extern int run_test_commercial_tenant_profile_2584();
extern int run_test_commit_readiness_score_2553();
extern int run_test_compact_policy_2500();
extern int run_test_cross_cow_drift_contract_2505();
extern int run_test_cross_cow_soft_migrate_2371();
extern int run_test_delta_truncate_goal_priority_2508();
extern int run_test_dep_graph_hybrid_cascade_2110();
extern int run_test_dispatch_required_effects_2152();
extern int run_test_dual_path_desync_hard_fail_2116();
extern int run_test_edsl_validate_or_refresh_2186();
extern int run_test_epoch_bump_invariant_2304();
extern int run_test_epoch_invariant_complete_2501();
extern int run_test_epoch_invariant_soft_prod_2541();
extern int run_test_epoch_invariant_walk_2366();
extern int run_test_escape_move_elision_gate_2263();
extern int run_test_eval_current_no_auto_fix_2484();
extern int run_test_frame_budget_cascade_isolation_2137();
extern int run_test_gc_closures_mtx_flush_sweep_2473();
extern int run_test_gc_coord_scope_2131();
extern int run_test_gc_heap_cells_clear_2486();
extern int run_test_hard_gate_full_strict_2145();
extern int run_test_hot_contract_placement_2435();
extern int run_test_hot_contract_unify_2142();
extern int run_test_hot_pass_hard_dod_2434();
extern int run_test_hot_pass_pure_wrap_2258();
extern int run_test_hot_strategy_2582();
extern int run_test_hygiene_checkpoint_2099();
extern int run_test_incremental_soundness_oracle_2113();
extern int run_test_instr_impact_minimal_dirty_2126();
extern int run_test_instruction_level_impact_partial_2109();
extern int run_test_isolation_audit_mid_2156();
extern int run_test_json_parse_number_exception_2480();
extern int run_test_json_parse_object_grow_2481();
extern int run_test_list_end_of_list_void_2482();
extern int run_test_load_cap_io_read_2485();
extern int run_test_lock_order_audit_2316();
extern int run_test_lock_order_audit_2354();
extern int run_test_lock_order_production_soft_2557();
extern int run_test_macro_cross_flat_hygiene_2235();
extern int run_test_macro_hygiene_limits_2101();
extern int run_test_mutate_capability_force_2052();
extern int run_test_mutate_type_gate_2219();
extern int run_test_orch_scope_child_2631();
extern int run_test_partial_cs_single_source_2262();
extern int run_test_persistent_typechecker_2220();
extern int run_test_post_compact_lifecycle_2436();
extern int run_test_predicate_meet_join_lattice_2148();
extern int run_test_production_hardening_985();
extern int run_test_production_safety_1047();
extern int run_test_production_safety_1097();
extern int run_test_production_security_defaults_2053();
extern int run_test_production_stability_1014();
extern int run_test_regex_redos_timeout_2479();
extern int run_test_rest_param_hygiene_2169();
extern int run_test_rest_param_nested_qq_hygiene_2239();
extern int run_test_reverify_expand_2356();
extern int run_test_rollback_by_marker_2237();
extern int run_test_selfevo_bugfix_941();
extern int run_test_shape_profiler_concurrency_2141();
extern int run_test_stable_ref_cow_refresh_failclosed_2393();
extern int run_test_stable_ref_export_validate_2404();
extern int run_test_stable_ref_tenant_mandate_2056();
extern int run_test_stable_ref_wire_v2_2198();
extern int run_test_stdlib_production_review_923();
extern int run_test_subtype_constraint_meet_2195();
extern int run_test_symbol_eq_2568();
extern int run_test_sys_open_path_harden_2487();
extern int run_test_timeout_repair_rich_roots_2548();
extern int run_test_truncate_commit_gate_2458();
extern int run_test_try_catch_bind_2567();
extern int run_test_type_dirty_txn_order_2516();
extern int run_test_value_tag_hot_path_2259();
extern int run_test_value_tag_hotpath_ban_2616();
extern int run_test_while_define_oneshot_2571();
extern int run_test_workspace_mtx_contention_2523();
extern int run_test_workspace_region_concurrency_2121();
extern int run_test_write_string_escape_2574();
extern int run_test_arena_adaptive_compact_2059();
extern int run_test_arena_compact_hook_stats_2381();
extern int run_test_arena_compact_notify_lifecycle_2438();
extern int run_test_arena_dtor_clears_hooks_2382();
extern int run_test_fixup_deltas_2392();
extern int run_test_force_compact_hard_mutex_2157();
extern int run_test_has_on_compact_hook_lock_2383();
extern int run_test_incremental_restamp_2061();
extern int run_test_pcv_exclusive_with_set_2140();
extern int run_test_pcv_tls_default_on_2521();
extern int run_test_pcv_tls_scratch_2406();
extern int run_test_pcv_unique_hotpath_2058();
extern int run_test_reset_slot_parent_edges_2412();
extern int run_test_restamp_sla_observability_2528();
extern int run_test_stable_ref_tenant_capture_2125();
extern int run_test_stable_ref_wire_endian_2395();
extern int run_test_subtree_dirty_bounds_2424();
extern int run_test_transaction_guard_2555();
extern int run_test_validate_node_no_abort_2390();
extern int run_test_workspace_isolation_wire_2073();
extern int run_test_hygiene_diagnostic_2167();
extern int run_test_ir_pod_phase4_2291();
extern int run_test_opcode_reflect_2289();
extern int run_test_reflect_isolation_2290();
extern int run_test_atomic_mark_bitvector_2117();
extern int run_test_gc_mark_size_inject_2084();
extern int run_test_issue_1990();
extern int run_test_issue_1991();
extern int run_test_issue_1992();
extern int run_test_issue_1993();
extern int run_test_mutate_mailbox_starvation_throttle_2587();
extern int run_test_spawn_quota_no_leak_2155();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_misc_issue_fold_batch (119 members) ===");

    std::println("\n──── test_adaptive_cascade_depth_partial_thr_2209 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_adaptive_cascade_depth_partial_thr_2209() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_adaptive_cascade_depth_partial_thr_2209 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_adaptive_cascade_depth_partial_thr_2209 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_adaptive_reverify_limit_2146 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_adaptive_reverify_limit_2146() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_adaptive_reverify_limit_2146 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_adaptive_reverify_limit_2146 ({} checks)", g_passed);
    }

    std::println("\n──── test_aether_denseness_residual_2578 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_aether_denseness_residual_2578() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_aether_denseness_residual_2578 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_aether_denseness_residual_2578 ({} checks)", g_passed);
    }

    std::println("\n──── test_audit_wal_force_multi_tenant_2150 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_audit_wal_force_multi_tenant_2150() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_audit_wal_force_multi_tenant_2150 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_audit_wal_force_multi_tenant_2150 ({} checks)", g_passed);
    }

    std::println("\n──── test_aura_sandbox_env_2076 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_aura_sandbox_env_2076() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_aura_sandbox_env_2076 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_aura_sandbox_env_2076 ({} checks)", g_passed);
    }

    std::println("\n──── test_bugfix_968 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_bugfix_968() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_bugfix_968 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_bugfix_968 ({} checks)", g_passed);
    }

    std::println("\n──── test_cascade_incremental_pass_suite_2044 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_cascade_incremental_pass_suite_2044() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_cascade_incremental_pass_suite_2044 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cascade_incremental_pass_suite_2044 ({} checks)", g_passed);
    }

    std::println("\n──── test_cascade_skip_metrics_2106 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_cascade_skip_metrics_2106() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_cascade_skip_metrics_2106 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cascade_skip_metrics_2106 ({} checks)", g_passed);
    }

    std::println("\n──── test_channel_rendezvous_2483 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_channel_rendezvous_2483() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_channel_rendezvous_2483 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_channel_rendezvous_2483 ({} checks)", g_passed);
    }

    std::println("\n──── test_command_line_cap_io_read_2478 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_command_line_cap_io_read_2478() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_command_line_cap_io_read_2478 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_command_line_cap_io_read_2478 ({} checks)", g_passed);
    }

    std::println("\n──── test_commercial_tenant_profile_2584 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_commercial_tenant_profile_2584() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_commercial_tenant_profile_2584 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_commercial_tenant_profile_2584 ({} checks)", g_passed);
    }

    std::println("\n──── test_commit_readiness_score_2553 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_commit_readiness_score_2553() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_commit_readiness_score_2553 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_commit_readiness_score_2553 ({} checks)", g_passed);
    }

    std::println("\n──── test_compact_policy_2500 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_compact_policy_2500() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_compact_policy_2500 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_compact_policy_2500 ({} checks)", g_passed);
    }

    std::println("\n──── test_cross_cow_drift_contract_2505 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_cross_cow_drift_contract_2505() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_cross_cow_drift_contract_2505 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cross_cow_drift_contract_2505 ({} checks)", g_passed);
    }

    std::println("\n──── test_cross_cow_soft_migrate_2371 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_cross_cow_soft_migrate_2371() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_cross_cow_soft_migrate_2371 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cross_cow_soft_migrate_2371 ({} checks)", g_passed);
    }

    std::println("\n──── test_delta_truncate_goal_priority_2508 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_delta_truncate_goal_priority_2508() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_delta_truncate_goal_priority_2508 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_delta_truncate_goal_priority_2508 ({} checks)", g_passed);
    }

    std::println("\n──── test_dep_graph_hybrid_cascade_2110 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dep_graph_hybrid_cascade_2110() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_dep_graph_hybrid_cascade_2110 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dep_graph_hybrid_cascade_2110 ({} checks)", g_passed);
    }

    std::println("\n──── test_dispatch_required_effects_2152 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dispatch_required_effects_2152() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_dispatch_required_effects_2152 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dispatch_required_effects_2152 ({} checks)", g_passed);
    }

    std::println("\n──── test_dual_path_desync_hard_fail_2116 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dual_path_desync_hard_fail_2116() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_dual_path_desync_hard_fail_2116 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dual_path_desync_hard_fail_2116 ({} checks)", g_passed);
    }

    std::println("\n──── test_edsl_validate_or_refresh_2186 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_edsl_validate_or_refresh_2186() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_edsl_validate_or_refresh_2186 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_edsl_validate_or_refresh_2186 ({} checks)", g_passed);
    }

    std::println("\n──── test_epoch_bump_invariant_2304 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_epoch_bump_invariant_2304() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_epoch_bump_invariant_2304 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_epoch_bump_invariant_2304 ({} checks)", g_passed);
    }

    std::println("\n──── test_epoch_invariant_complete_2501 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_epoch_invariant_complete_2501() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_epoch_invariant_complete_2501 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_epoch_invariant_complete_2501 ({} checks)", g_passed);
    }

    std::println("\n──── test_epoch_invariant_soft_prod_2541 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_epoch_invariant_soft_prod_2541() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_epoch_invariant_soft_prod_2541 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_epoch_invariant_soft_prod_2541 ({} checks)", g_passed);
    }

    std::println("\n──── test_epoch_invariant_walk_2366 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_epoch_invariant_walk_2366() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_epoch_invariant_walk_2366 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_epoch_invariant_walk_2366 ({} checks)", g_passed);
    }

    std::println("\n──── test_escape_move_elision_gate_2263 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_escape_move_elision_gate_2263() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_escape_move_elision_gate_2263 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_escape_move_elision_gate_2263 ({} checks)", g_passed);
    }

    std::println("\n──── test_eval_current_no_auto_fix_2484 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_eval_current_no_auto_fix_2484() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_eval_current_no_auto_fix_2484 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_eval_current_no_auto_fix_2484 ({} checks)", g_passed);
    }

    std::println("\n──── test_frame_budget_cascade_isolation_2137 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_frame_budget_cascade_isolation_2137() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_frame_budget_cascade_isolation_2137 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_frame_budget_cascade_isolation_2137 ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_closures_mtx_flush_sweep_2473 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_closures_mtx_flush_sweep_2473() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_gc_closures_mtx_flush_sweep_2473 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_closures_mtx_flush_sweep_2473 ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_coord_scope_2131 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_coord_scope_2131() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_coord_scope_2131 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_coord_scope_2131 ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_heap_cells_clear_2486 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_heap_cells_clear_2486() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_heap_cells_clear_2486 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_heap_cells_clear_2486 ({} checks)", g_passed);
    }

    std::println("\n──── test_hard_gate_full_strict_2145 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hard_gate_full_strict_2145() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hard_gate_full_strict_2145 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hard_gate_full_strict_2145 ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_contract_placement_2435 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_contract_placement_2435() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_contract_placement_2435 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_contract_placement_2435 ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_contract_unify_2142 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_contract_unify_2142() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_contract_unify_2142 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_contract_unify_2142 ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_pass_hard_dod_2434 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_pass_hard_dod_2434() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_pass_hard_dod_2434 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_pass_hard_dod_2434 ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_pass_pure_wrap_2258 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_pass_pure_wrap_2258() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_pass_pure_wrap_2258 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_pass_pure_wrap_2258 ({} checks)", g_passed);
    }

    std::println("\n──── test_hot_strategy_2582 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hot_strategy_2582() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hot_strategy_2582 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hot_strategy_2582 ({} checks)", g_passed);
    }

    std::println("\n──── test_hygiene_checkpoint_2099 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hygiene_checkpoint_2099() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hygiene_checkpoint_2099 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hygiene_checkpoint_2099 ({} checks)", g_passed);
    }

    std::println("\n──── test_incremental_soundness_oracle_2113 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_incremental_soundness_oracle_2113() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_incremental_soundness_oracle_2113 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_incremental_soundness_oracle_2113 ({} checks)", g_passed);
    }

    std::println("\n──── test_instr_impact_minimal_dirty_2126 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_instr_impact_minimal_dirty_2126() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_instr_impact_minimal_dirty_2126 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_instr_impact_minimal_dirty_2126 ({} checks)", g_passed);
    }

    std::println("\n──── test_instruction_level_impact_partial_2109 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_instruction_level_impact_partial_2109() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_instruction_level_impact_partial_2109 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_instruction_level_impact_partial_2109 ({} checks)", g_passed);
    }

    std::println("\n──── test_isolation_audit_mid_2156 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_isolation_audit_mid_2156() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_isolation_audit_mid_2156 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_isolation_audit_mid_2156 ({} checks)", g_passed);
    }

    std::println("\n──── test_json_parse_number_exception_2480 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_json_parse_number_exception_2480() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_json_parse_number_exception_2480 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_json_parse_number_exception_2480 ({} checks)", g_passed);
    }

    std::println("\n──── test_json_parse_object_grow_2481 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_json_parse_object_grow_2481() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_json_parse_object_grow_2481 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_json_parse_object_grow_2481 ({} checks)", g_passed);
    }

    std::println("\n──── test_list_end_of_list_void_2482 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_list_end_of_list_void_2482() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_list_end_of_list_void_2482 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_list_end_of_list_void_2482 ({} checks)", g_passed);
    }

    std::println("\n──── test_load_cap_io_read_2485 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_load_cap_io_read_2485() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_load_cap_io_read_2485 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_load_cap_io_read_2485 ({} checks)", g_passed);
    }

    std::println("\n──── test_lock_order_audit_2316 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_lock_order_audit_2316() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_lock_order_audit_2316 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_lock_order_audit_2316 ({} checks)", g_passed);
    }

    std::println("\n──── test_lock_order_audit_2354 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_lock_order_audit_2354() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_lock_order_audit_2354 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_lock_order_audit_2354 ({} checks)", g_passed);
    }

    std::println("\n──── test_lock_order_production_soft_2557 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_lock_order_production_soft_2557() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_lock_order_production_soft_2557 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_lock_order_production_soft_2557 ({} checks)", g_passed);
    }

    std::println("\n──── test_macro_cross_flat_hygiene_2235 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_macro_cross_flat_hygiene_2235() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_macro_cross_flat_hygiene_2235 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_macro_cross_flat_hygiene_2235 ({} checks)", g_passed);
    }

    std::println("\n──── test_macro_hygiene_limits_2101 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_macro_hygiene_limits_2101() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_macro_hygiene_limits_2101 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_macro_hygiene_limits_2101 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutate_capability_force_2052 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutate_capability_force_2052() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutate_capability_force_2052 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutate_capability_force_2052 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutate_type_gate_2219 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutate_type_gate_2219() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutate_type_gate_2219 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutate_type_gate_2219 ({} checks)", g_passed);
    }

    std::println("\n──── test_orch_scope_child_2631 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_orch_scope_child_2631() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_orch_scope_child_2631 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_orch_scope_child_2631 ({} checks)", g_passed);
    }

    std::println("\n──── test_partial_cs_single_source_2262 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_partial_cs_single_source_2262() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_partial_cs_single_source_2262 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_partial_cs_single_source_2262 ({} checks)", g_passed);
    }

    std::println("\n──── test_persistent_typechecker_2220 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_persistent_typechecker_2220() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_persistent_typechecker_2220 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_persistent_typechecker_2220 ({} checks)", g_passed);
    }

    std::println("\n──── test_post_compact_lifecycle_2436 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_post_compact_lifecycle_2436() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_post_compact_lifecycle_2436 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_post_compact_lifecycle_2436 ({} checks)", g_passed);
    }

    std::println("\n──── test_predicate_meet_join_lattice_2148 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_predicate_meet_join_lattice_2148() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_predicate_meet_join_lattice_2148 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_predicate_meet_join_lattice_2148 ({} checks)", g_passed);
    }

    std::println("\n──── test_production_hardening_985 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_hardening_985() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_production_hardening_985 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_production_hardening_985 ({} checks)", g_passed);
    }

    std::println("\n──── test_production_safety_1047 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_safety_1047() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_production_safety_1047 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_production_safety_1047 ({} checks)", g_passed);
    }

    std::println("\n──── test_production_safety_1097 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_safety_1097() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_production_safety_1097 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_production_safety_1097 ({} checks)", g_passed);
    }

    std::println("\n──── test_production_security_defaults_2053 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_security_defaults_2053() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_production_security_defaults_2053 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_production_security_defaults_2053 ({} checks)", g_passed);
    }

    std::println("\n──── test_production_stability_1014 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_stability_1014() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_production_stability_1014 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_production_stability_1014 ({} checks)", g_passed);
    }

    std::println("\n──── test_regex_redos_timeout_2479 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_regex_redos_timeout_2479() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_regex_redos_timeout_2479 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_regex_redos_timeout_2479 ({} checks)", g_passed);
    }

    std::println("\n──── test_rest_param_hygiene_2169 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_rest_param_hygiene_2169() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_rest_param_hygiene_2169 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_rest_param_hygiene_2169 ({} checks)", g_passed);
    }

    std::println("\n──── test_rest_param_nested_qq_hygiene_2239 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_rest_param_nested_qq_hygiene_2239() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_rest_param_nested_qq_hygiene_2239 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_rest_param_nested_qq_hygiene_2239 ({} checks)", g_passed);
    }

    std::println("\n──── test_reverify_expand_2356 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reverify_expand_2356() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_reverify_expand_2356 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_reverify_expand_2356 ({} checks)", g_passed);
    }

    std::println("\n──── test_rollback_by_marker_2237 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_rollback_by_marker_2237() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_rollback_by_marker_2237 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_rollback_by_marker_2237 ({} checks)", g_passed);
    }

    std::println("\n──── test_selfevo_bugfix_941 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_selfevo_bugfix_941() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_selfevo_bugfix_941 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_selfevo_bugfix_941 ({} checks)", g_passed);
    }

    std::println("\n──── test_shape_profiler_concurrency_2141 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_shape_profiler_concurrency_2141() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_shape_profiler_concurrency_2141 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_shape_profiler_concurrency_2141 ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_cow_refresh_failclosed_2393 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_cow_refresh_failclosed_2393() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stable_ref_cow_refresh_failclosed_2393 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_cow_refresh_failclosed_2393 ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_export_validate_2404 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_export_validate_2404() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_stable_ref_export_validate_2404 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_export_validate_2404 ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_tenant_mandate_2056 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_tenant_mandate_2056() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_stable_ref_tenant_mandate_2056 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_tenant_mandate_2056 ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_wire_v2_2198 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_wire_v2_2198() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stable_ref_wire_v2_2198 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_wire_v2_2198 ({} checks)", g_passed);
    }

    std::println("\n──── test_stdlib_production_review_923 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stdlib_production_review_923() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stdlib_production_review_923 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stdlib_production_review_923 ({} checks)", g_passed);
    }

    std::println("\n──── test_subtype_constraint_meet_2195 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_subtype_constraint_meet_2195() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_subtype_constraint_meet_2195 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_subtype_constraint_meet_2195 ({} checks)", g_passed);
    }

    std::println("\n──── test_symbol_eq_2568 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_symbol_eq_2568() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_symbol_eq_2568 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_symbol_eq_2568 ({} checks)", g_passed);
    }

    std::println("\n──── test_sys_open_path_harden_2487 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_sys_open_path_harden_2487() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_sys_open_path_harden_2487 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_sys_open_path_harden_2487 ({} checks)", g_passed);
    }

    std::println("\n──── test_timeout_repair_rich_roots_2548 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_timeout_repair_rich_roots_2548() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_timeout_repair_rich_roots_2548 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_timeout_repair_rich_roots_2548 ({} checks)", g_passed);
    }

    std::println("\n──── test_truncate_commit_gate_2458 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_truncate_commit_gate_2458() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_truncate_commit_gate_2458 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_truncate_commit_gate_2458 ({} checks)", g_passed);
    }

    std::println("\n──── test_try_catch_bind_2567 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_try_catch_bind_2567() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_try_catch_bind_2567 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_try_catch_bind_2567 ({} checks)", g_passed);
    }

    std::println("\n──── test_type_dirty_txn_order_2516 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_dirty_txn_order_2516() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_dirty_txn_order_2516 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_dirty_txn_order_2516 ({} checks)", g_passed);
    }

    std::println("\n──── test_value_tag_hot_path_2259 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_value_tag_hot_path_2259() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_value_tag_hot_path_2259 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_value_tag_hot_path_2259 ({} checks)", g_passed);
    }

    std::println("\n──── test_value_tag_hotpath_ban_2616 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_value_tag_hotpath_ban_2616() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_value_tag_hotpath_ban_2616 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_value_tag_hotpath_ban_2616 ({} checks)", g_passed);
    }

    std::println("\n──── test_while_define_oneshot_2571 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_while_define_oneshot_2571() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_while_define_oneshot_2571 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_while_define_oneshot_2571 ({} checks)", g_passed);
    }

    std::println("\n──── test_workspace_mtx_contention_2523 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_workspace_mtx_contention_2523() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_workspace_mtx_contention_2523 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_workspace_mtx_contention_2523 ({} checks)", g_passed);
    }

    std::println("\n──── test_workspace_region_concurrency_2121 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_workspace_region_concurrency_2121() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_workspace_region_concurrency_2121 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_workspace_region_concurrency_2121 ({} checks)", g_passed);
    }

    std::println("\n──── test_write_string_escape_2574 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_write_string_escape_2574() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_write_string_escape_2574 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_write_string_escape_2574 ({} checks)", g_passed);
    }

    std::println("\n──── test_arena_adaptive_compact_2059 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_arena_adaptive_compact_2059() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_arena_adaptive_compact_2059 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_arena_adaptive_compact_2059 ({} checks)", g_passed);
    }

    std::println("\n──── test_arena_compact_hook_stats_2381 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_arena_compact_hook_stats_2381() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_arena_compact_hook_stats_2381 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_arena_compact_hook_stats_2381 ({} checks)", g_passed);
    }

    std::println("\n──── test_arena_compact_notify_lifecycle_2438 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_arena_compact_notify_lifecycle_2438() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_arena_compact_notify_lifecycle_2438 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_arena_compact_notify_lifecycle_2438 ({} checks)", g_passed);
    }

    std::println("\n──── test_arena_dtor_clears_hooks_2382 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_arena_dtor_clears_hooks_2382() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_arena_dtor_clears_hooks_2382 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_arena_dtor_clears_hooks_2382 ({} checks)", g_passed);
    }

    std::println("\n──── test_fixup_deltas_2392 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_fixup_deltas_2392() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_fixup_deltas_2392 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_fixup_deltas_2392 ({} checks)", g_passed);
    }

    std::println("\n──── test_force_compact_hard_mutex_2157 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_force_compact_hard_mutex_2157() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_force_compact_hard_mutex_2157 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_force_compact_hard_mutex_2157 ({} checks)", g_passed);
    }

    std::println("\n──── test_has_on_compact_hook_lock_2383 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_has_on_compact_hook_lock_2383() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_has_on_compact_hook_lock_2383 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_has_on_compact_hook_lock_2383 ({} checks)", g_passed);
    }

    std::println("\n──── test_incremental_restamp_2061 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_incremental_restamp_2061() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_incremental_restamp_2061 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_incremental_restamp_2061 ({} checks)", g_passed);
    }

    std::println("\n──── test_pcv_exclusive_with_set_2140 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_pcv_exclusive_with_set_2140() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_pcv_exclusive_with_set_2140 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_pcv_exclusive_with_set_2140 ({} checks)", g_passed);
    }

    std::println("\n──── test_pcv_tls_default_on_2521 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_pcv_tls_default_on_2521() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_pcv_tls_default_on_2521 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_pcv_tls_default_on_2521 ({} checks)", g_passed);
    }

    std::println("\n──── test_pcv_tls_scratch_2406 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_pcv_tls_scratch_2406() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_pcv_tls_scratch_2406 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_pcv_tls_scratch_2406 ({} checks)", g_passed);
    }

    std::println("\n──── test_pcv_unique_hotpath_2058 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_pcv_unique_hotpath_2058() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_pcv_unique_hotpath_2058 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_pcv_unique_hotpath_2058 ({} checks)", g_passed);
    }

    std::println("\n──── test_reset_slot_parent_edges_2412 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reset_slot_parent_edges_2412() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_reset_slot_parent_edges_2412 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_reset_slot_parent_edges_2412 ({} checks)", g_passed);
    }

    std::println("\n──── test_restamp_sla_observability_2528 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_restamp_sla_observability_2528() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_restamp_sla_observability_2528 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_restamp_sla_observability_2528 ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_tenant_capture_2125 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_tenant_capture_2125() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_stable_ref_tenant_capture_2125 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_tenant_capture_2125 ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_wire_endian_2395 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_wire_endian_2395() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stable_ref_wire_endian_2395 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_wire_endian_2395 ({} checks)", g_passed);
    }

    std::println("\n──── test_subtree_dirty_bounds_2424 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_subtree_dirty_bounds_2424() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_subtree_dirty_bounds_2424 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_subtree_dirty_bounds_2424 ({} checks)", g_passed);
    }

    std::println("\n──── test_transaction_guard_2555 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_transaction_guard_2555() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_transaction_guard_2555 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_transaction_guard_2555 ({} checks)", g_passed);
    }

    std::println("\n──── test_validate_node_no_abort_2390 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_validate_node_no_abort_2390() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_validate_node_no_abort_2390 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_validate_node_no_abort_2390 ({} checks)", g_passed);
    }

    std::println("\n──── test_workspace_isolation_wire_2073 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_workspace_isolation_wire_2073() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_workspace_isolation_wire_2073 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_workspace_isolation_wire_2073 ({} checks)", g_passed);
    }

    std::println("\n──── test_hygiene_diagnostic_2167 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hygiene_diagnostic_2167() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hygiene_diagnostic_2167 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hygiene_diagnostic_2167 ({} checks)", g_passed);
    }

    std::println("\n──── test_ir_pod_phase4_2291 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_ir_pod_phase4_2291() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_ir_pod_phase4_2291 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_ir_pod_phase4_2291 ({} checks)", g_passed);
    }

    std::println("\n──── test_opcode_reflect_2289 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_opcode_reflect_2289() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_opcode_reflect_2289 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_opcode_reflect_2289 ({} checks)", g_passed);
    }

    std::println("\n──── test_reflect_isolation_2290 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reflect_isolation_2290() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_reflect_isolation_2290 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_reflect_isolation_2290 ({} checks)", g_passed);
    }

    std::println("\n──── test_atomic_mark_bitvector_2117 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_atomic_mark_bitvector_2117() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_atomic_mark_bitvector_2117 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_atomic_mark_bitvector_2117 ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_mark_size_inject_2084 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_mark_size_inject_2084() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_mark_size_inject_2084 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_mark_size_inject_2084 ({} checks)", g_passed);
    }

    std::println("\n──── test_issue_1990 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1990() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1990 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_issue_1990 ({} checks)", g_passed);
    }

    std::println("\n──── test_issue_1991 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1991() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1991 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_issue_1991 ({} checks)", g_passed);
    }

    std::println("\n──── test_issue_1992 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1992() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1992 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_issue_1992 ({} checks)", g_passed);
    }

    std::println("\n──── test_issue_1993 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_issue_1993() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_issue_1993 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_issue_1993 ({} checks)", g_passed);
    }

    std::println("\n──── test_mutate_mailbox_starvation_throttle_2587 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_mutate_mailbox_starvation_throttle_2587() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_mutate_mailbox_starvation_throttle_2587 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_mutate_mailbox_starvation_throttle_2587 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_spawn_quota_no_leak_2155 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_spawn_quota_no_leak_2155() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_spawn_quota_no_leak_2155 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_spawn_quota_no_leak_2155 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
