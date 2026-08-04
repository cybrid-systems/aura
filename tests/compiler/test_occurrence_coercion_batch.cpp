// test_occurrence_coercion_batch.cpp — thematic multi-TU batch
// Occurrence / cone / coercion / type gates
// Members export run_<name>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.
// Policy: tests/HOMES.md — extend this batch, do not add test_*_<issue>.cpp.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_adt_exhaustiveness_audit_2223();
extern int run_test_adt_hard_gate_exhaustiveness_2264();
extern int run_test_adt_match_goal_table_2564();
extern int run_test_batch_dirty_cascade_2522();
extern int run_test_batch_dirty_discipline_2615();
extern int run_test_bidirectional_match_check_2348();
extern int run_test_blame_complete_commit_gate_2221();
extern int run_test_blame_soft_recover_2561();
extern int run_test_boundary_solve_hard_gate_2260();
extern int run_test_castop_density_closed_loop_2459();
extern int run_test_castop_density_hard_2358();
extern int run_test_castop_typed_meta_2624();
extern int run_test_coercion_ban_weak_ir_2261();
extern int run_test_coercion_dual_require_2562();
extern int run_test_coercion_prov_slo_2558();
extern int run_test_coercion_provenance_fast_strict_2147();
extern int run_test_coercion_provenance_miss_force_audit_2102();
extern int run_test_coercion_reject_production_defaults_2185();
extern int run_test_coercion_unify_incomplete_skip_2620();
extern int run_test_composite_auto_partial_from_cone_2610();
extern int run_test_composite_commit_cs_reuse_2180();
extern int run_test_composite_cs_signature_matrix_2509();
extern int run_test_composite_txn_commit_2105();
extern int run_test_dead_coercion_columnar_2431();
extern int run_test_dead_coercion_dirty_cone_2556();
extern int run_test_dead_coercion_layered_2282();
extern int run_test_instance_constraint_depth_cap_2607();
extern int run_test_occurrence_cache_key_2461();
extern int run_test_occurrence_dirty_key_authority_2622();
extern int run_test_occurrence_goal_epoch_table_2278();
extern int run_test_occurrence_goal_persist_rehydrate_2608();
extern int run_test_partial_cone_cap_2560();
extern int run_test_partial_cone_commit_gate_2621();
extern int run_test_solve_delta_unresolved_export_2107();
extern int run_test_type_dep_epoch_prune_2355();
extern int run_test_type_dep_partial_merge_2283();
extern int run_test_type_dirty_cone_dep_graph_2191();
extern int run_test_type_linear_commit_health_2613();
extern int run_test_type_system_health_2350();
extern int run_test_type_system_health_next_action_2462();
extern int run_test_type_timeout_repair_2284();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_occurrence_coercion_batch (41 members) ===");

    std::println("\n──── test_adt_exhaustiveness_audit_2223 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_adt_exhaustiveness_audit_2223() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_adt_exhaustiveness_audit_2223 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_adt_exhaustiveness_audit_2223 ({} checks)", g_passed);
    }

    std::println("\n──── test_adt_hard_gate_exhaustiveness_2264 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_adt_hard_gate_exhaustiveness_2264() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_adt_hard_gate_exhaustiveness_2264 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_adt_hard_gate_exhaustiveness_2264 ({} checks)", g_passed);
    }

    std::println("\n──── test_adt_match_goal_table_2564 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_adt_match_goal_table_2564() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_adt_match_goal_table_2564 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_adt_match_goal_table_2564 ({} checks)", g_passed);
    }

    std::println("\n──── test_batch_dirty_cascade_2522 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_batch_dirty_cascade_2522() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_batch_dirty_cascade_2522 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_batch_dirty_cascade_2522 ({} checks)", g_passed);
    }

    std::println("\n──── test_batch_dirty_discipline_2615 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_batch_dirty_discipline_2615() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_batch_dirty_discipline_2615 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_batch_dirty_discipline_2615 ({} checks)", g_passed);
    }

    std::println("\n──── test_bidirectional_match_check_2348 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_bidirectional_match_check_2348() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_bidirectional_match_check_2348 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_bidirectional_match_check_2348 ({} checks)", g_passed);
    }

    std::println("\n──── test_blame_complete_commit_gate_2221 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_blame_complete_commit_gate_2221() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_blame_complete_commit_gate_2221 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_blame_complete_commit_gate_2221 ({} checks)", g_passed);
    }

    std::println("\n──── test_blame_soft_recover_2561 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_blame_soft_recover_2561() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_blame_soft_recover_2561 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_blame_soft_recover_2561 ({} checks)", g_passed);
    }

    std::println("\n──── test_boundary_solve_hard_gate_2260 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_boundary_solve_hard_gate_2260() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_boundary_solve_hard_gate_2260 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_boundary_solve_hard_gate_2260 ({} checks)", g_passed);
    }

    std::println("\n──── test_castop_density_closed_loop_2459 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_castop_density_closed_loop_2459() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_castop_density_closed_loop_2459 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_castop_density_closed_loop_2459 ({} checks)", g_passed);
    }

    std::println("\n──── test_castop_density_hard_2358 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_castop_density_hard_2358() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_castop_density_hard_2358 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_castop_density_hard_2358 ({} checks)", g_passed);
    }

    std::println("\n──── test_castop_typed_meta_2624 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_castop_typed_meta_2624() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_castop_typed_meta_2624 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_castop_typed_meta_2624 ({} checks)", g_passed);
    }

    std::println("\n──── test_coercion_ban_weak_ir_2261 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_ban_weak_ir_2261() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_ban_weak_ir_2261 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_ban_weak_ir_2261 ({} checks)", g_passed);
    }

    std::println("\n──── test_coercion_dual_require_2562 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_dual_require_2562() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_dual_require_2562 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_dual_require_2562 ({} checks)", g_passed);
    }

    std::println("\n──── test_coercion_prov_slo_2558 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_prov_slo_2558() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_prov_slo_2558 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_prov_slo_2558 ({} checks)", g_passed);
    }

    std::println("\n──── test_coercion_provenance_fast_strict_2147 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_provenance_fast_strict_2147() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_coercion_provenance_fast_strict_2147 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_provenance_fast_strict_2147 ({} checks)", g_passed);
    }

    std::println("\n──── test_coercion_provenance_miss_force_audit_2102 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_provenance_miss_force_audit_2102() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_provenance_miss_force_audit_2102 (checks: {} "
                     "passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_provenance_miss_force_audit_2102 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_coercion_reject_production_defaults_2185 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_reject_production_defaults_2185() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_reject_production_defaults_2185 (checks: {} "
                     "passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_reject_production_defaults_2185 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_coercion_unify_incomplete_skip_2620 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_unify_incomplete_skip_2620() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_coercion_unify_incomplete_skip_2620 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_unify_incomplete_skip_2620 ({} checks)", g_passed);
    }

    std::println("\n──── test_composite_auto_partial_from_cone_2610 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_composite_auto_partial_from_cone_2610() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_composite_auto_partial_from_cone_2610 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_composite_auto_partial_from_cone_2610 ({} checks)", g_passed);
    }

    std::println("\n──── test_composite_commit_cs_reuse_2180 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_composite_commit_cs_reuse_2180() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_composite_commit_cs_reuse_2180 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_composite_commit_cs_reuse_2180 ({} checks)", g_passed);
    }

    std::println("\n──── test_composite_cs_signature_matrix_2509 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_composite_cs_signature_matrix_2509() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_composite_cs_signature_matrix_2509 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_composite_cs_signature_matrix_2509 ({} checks)", g_passed);
    }

    std::println("\n──── test_composite_txn_commit_2105 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_composite_txn_commit_2105() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_composite_txn_commit_2105 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_composite_txn_commit_2105 ({} checks)", g_passed);
    }

    std::println("\n──── test_dead_coercion_columnar_2431 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dead_coercion_columnar_2431() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dead_coercion_columnar_2431 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dead_coercion_columnar_2431 ({} checks)", g_passed);
    }

    std::println("\n──── test_dead_coercion_dirty_cone_2556 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dead_coercion_dirty_cone_2556() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_dead_coercion_dirty_cone_2556 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dead_coercion_dirty_cone_2556 ({} checks)", g_passed);
    }

    std::println("\n──── test_dead_coercion_layered_2282 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dead_coercion_layered_2282() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dead_coercion_layered_2282 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dead_coercion_layered_2282 ({} checks)", g_passed);
    }

    std::println("\n──── test_instance_constraint_depth_cap_2607 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_instance_constraint_depth_cap_2607() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_instance_constraint_depth_cap_2607 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_instance_constraint_depth_cap_2607 ({} checks)", g_passed);
    }

    std::println("\n──── test_occurrence_cache_key_2461 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_occurrence_cache_key_2461() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_occurrence_cache_key_2461 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_occurrence_cache_key_2461 ({} checks)", g_passed);
    }

    std::println("\n──── test_occurrence_dirty_key_authority_2622 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_occurrence_dirty_key_authority_2622() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_occurrence_dirty_key_authority_2622 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_occurrence_dirty_key_authority_2622 ({} checks)", g_passed);
    }

    std::println("\n──── test_occurrence_goal_epoch_table_2278 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_occurrence_goal_epoch_table_2278() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_occurrence_goal_epoch_table_2278 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_occurrence_goal_epoch_table_2278 ({} checks)", g_passed);
    }

    std::println("\n──── test_occurrence_goal_persist_rehydrate_2608 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_occurrence_goal_persist_rehydrate_2608() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_occurrence_goal_persist_rehydrate_2608 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_occurrence_goal_persist_rehydrate_2608 ({} checks)", g_passed);
    }

    std::println("\n──── test_partial_cone_cap_2560 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_partial_cone_cap_2560() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_partial_cone_cap_2560 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_partial_cone_cap_2560 ({} checks)", g_passed);
    }

    std::println("\n──── test_partial_cone_commit_gate_2621 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_partial_cone_commit_gate_2621() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_partial_cone_commit_gate_2621 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_partial_cone_commit_gate_2621 ({} checks)", g_passed);
    }

    std::println("\n──── test_solve_delta_unresolved_export_2107 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_solve_delta_unresolved_export_2107() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_solve_delta_unresolved_export_2107 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_solve_delta_unresolved_export_2107 ({} checks)", g_passed);
    }

    std::println("\n──── test_type_dep_epoch_prune_2355 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_dep_epoch_prune_2355() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_dep_epoch_prune_2355 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_dep_epoch_prune_2355 ({} checks)", g_passed);
    }

    std::println("\n──── test_type_dep_partial_merge_2283 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_dep_partial_merge_2283() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_dep_partial_merge_2283 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_dep_partial_merge_2283 ({} checks)", g_passed);
    }

    std::println("\n──── test_type_dirty_cone_dep_graph_2191 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_dirty_cone_dep_graph_2191() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_type_dirty_cone_dep_graph_2191 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_dirty_cone_dep_graph_2191 ({} checks)", g_passed);
    }

    std::println("\n──── test_type_linear_commit_health_2613 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_linear_commit_health_2613() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_type_linear_commit_health_2613 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_linear_commit_health_2613 ({} checks)", g_passed);
    }

    std::println("\n──── test_type_system_health_2350 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_system_health_2350() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_system_health_2350 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_system_health_2350 ({} checks)", g_passed);
    }

    std::println("\n──── test_type_system_health_next_action_2462 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_system_health_next_action_2462() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_type_system_health_next_action_2462 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_system_health_next_action_2462 ({} checks)", g_passed);
    }

    std::println("\n──── test_type_timeout_repair_2284 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_timeout_repair_2284() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_timeout_repair_2284 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_timeout_repair_2284 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
