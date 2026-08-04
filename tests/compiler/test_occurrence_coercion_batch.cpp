// test_occurrence_coercion_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_adt_exhaustiveness_audit();
extern int run_test_adt_hard_gate_exhaustiveness();
extern int run_test_adt_match_goal_table();
extern int run_test_batch_dirty_cascade();
extern int run_test_batch_dirty_discipline();
extern int run_test_bidirectional_match_check();
extern int run_test_blame_complete_commit_gate();
extern int run_test_blame_soft_recover();
extern int run_test_boundary_solve_hard_gate();
extern int run_test_castop_density_closed_loop();
extern int run_test_castop_density_hard();
extern int run_test_castop_typed_meta();
extern int run_test_coercion_ban_weak_ir();
extern int run_test_coercion_dual_require();
extern int run_test_coercion_evidence_loss_slo();
extern int run_test_coercion_prov_slo();
extern int run_test_coercion_provenance_fast_strict();
extern int run_test_coercion_provenance_miss_force_audit();
extern int run_test_coercion_reject_production_defaults();
extern int run_test_coercion_unify_incomplete_skip();
extern int run_test_composite_auto_partial_from_cone();
extern int run_test_composite_commit_cs_reuse();
extern int run_test_composite_cs_signature_matrix();
extern int run_test_composite_txn_commit();
extern int run_test_dead_coercion_columnar();
extern int run_test_dead_coercion_dirty_cone();
extern int run_test_dead_coercion_layered();
extern int run_test_instance_constraint_depth_cap();
extern int run_test_occurrence_cache_key();
extern int run_test_occurrence_dirty_key_authority();
extern int run_test_occurrence_goal_epoch_table();
extern int run_test_occurrence_goal_persist_rehydrate();
extern int run_test_occurrence_goal_vacuous_solve_prevent();
extern int run_test_partial_cone_cap();
extern int run_test_partial_cone_commit_gate();
extern int run_test_solve_delta_unresolved_export();
extern int run_test_type_dep_epoch_prune();
extern int run_test_type_dep_partial_merge();
extern int run_test_type_dirty_cone_dep_graph();
extern int run_test_type_linear_commit_health();
extern int run_test_type_system_health();
extern int run_test_type_system_health_next_action();
extern int run_test_type_timeout_repair();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_occurrence_coercion_batch (43 members) ===");

    // #2648 early: independent Soft evidence-loss SLO (avoids ADT batch abort
    // masking later members during incremental verify).
    std::println("\n──── test_coercion_evidence_loss_slo ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_evidence_loss_slo() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_evidence_loss_slo ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_evidence_loss_slo ({} checks)", g_passed);
    }

    std::println("\n──── test_adt_exhaustiveness_audit ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_adt_exhaustiveness_audit() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_adt_exhaustiveness_audit ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_adt_exhaustiveness_audit ({} checks)", g_passed);
    }

    std::println("\n──── test_adt_hard_gate_exhaustiveness ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_adt_hard_gate_exhaustiveness() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_adt_hard_gate_exhaustiveness ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_adt_hard_gate_exhaustiveness ({} checks)", g_passed);
    }

    std::println("\n──── test_adt_match_goal_table ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_adt_match_goal_table() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_adt_match_goal_table ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_adt_match_goal_table ({} checks)", g_passed);
    }

    std::println("\n──── test_batch_dirty_cascade ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_batch_dirty_cascade() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_batch_dirty_cascade ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_batch_dirty_cascade ({} checks)", g_passed);
    }

    std::println("\n──── test_batch_dirty_discipline ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_batch_dirty_discipline() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_batch_dirty_discipline ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_batch_dirty_discipline ({} checks)", g_passed);
    }

    std::println("\n──── test_bidirectional_match_check ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_bidirectional_match_check() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_bidirectional_match_check ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_bidirectional_match_check ({} checks)", g_passed);
    }

    std::println("\n──── test_blame_complete_commit_gate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_blame_complete_commit_gate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_blame_complete_commit_gate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_blame_complete_commit_gate ({} checks)", g_passed);
    }

    std::println("\n──── test_blame_soft_recover ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_blame_soft_recover() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_blame_soft_recover ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_blame_soft_recover ({} checks)", g_passed);
    }

    std::println("\n──── test_boundary_solve_hard_gate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_boundary_solve_hard_gate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_boundary_solve_hard_gate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_boundary_solve_hard_gate ({} checks)", g_passed);
    }

    std::println("\n──── test_castop_density_closed_loop ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_castop_density_closed_loop() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_castop_density_closed_loop ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_castop_density_closed_loop ({} checks)", g_passed);
    }

    std::println("\n──── test_castop_density_hard ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_castop_density_hard() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_castop_density_hard ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_castop_density_hard ({} checks)", g_passed);
    }

    std::println("\n──── test_castop_typed_meta ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_castop_typed_meta() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_castop_typed_meta ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_castop_typed_meta ({} checks)", g_passed);
    }

    std::println("\n──── test_coercion_ban_weak_ir ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_ban_weak_ir() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_ban_weak_ir ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_ban_weak_ir ({} checks)", g_passed);
    }

    std::println("\n──── test_coercion_dual_require ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_dual_require() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_dual_require ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_dual_require ({} checks)", g_passed);
    }

    std::println("\n──── test_coercion_prov_slo ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_prov_slo() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_prov_slo ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_prov_slo ({} checks)", g_passed);
    }

    std::println("\n──── test_coercion_provenance_fast_strict ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_provenance_fast_strict() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_provenance_fast_strict ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_provenance_fast_strict ({} checks)", g_passed);
    }

    std::println("\n──── test_coercion_provenance_miss_force_audit ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_provenance_miss_force_audit() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_provenance_miss_force_audit ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_provenance_miss_force_audit ({} checks)", g_passed);
    }

    std::println("\n──── test_coercion_reject_production_defaults ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_reject_production_defaults() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_reject_production_defaults ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_reject_production_defaults ({} checks)", g_passed);
    }

    std::println("\n──── test_coercion_unify_incomplete_skip ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_unify_incomplete_skip() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_unify_incomplete_skip ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_unify_incomplete_skip ({} checks)", g_passed);
    }

    std::println("\n──── test_composite_auto_partial_from_cone ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_composite_auto_partial_from_cone() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_composite_auto_partial_from_cone ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_composite_auto_partial_from_cone ({} checks)", g_passed);
    }

    std::println("\n──── test_composite_commit_cs_reuse ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_composite_commit_cs_reuse() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_composite_commit_cs_reuse ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_composite_commit_cs_reuse ({} checks)", g_passed);
    }

    std::println("\n──── test_composite_cs_signature_matrix ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_composite_cs_signature_matrix() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_composite_cs_signature_matrix ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_composite_cs_signature_matrix ({} checks)", g_passed);
    }

    std::println("\n──── test_composite_txn_commit ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_composite_txn_commit() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_composite_txn_commit ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_composite_txn_commit ({} checks)", g_passed);
    }

    std::println("\n──── test_dead_coercion_columnar ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dead_coercion_columnar() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dead_coercion_columnar ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dead_coercion_columnar ({} checks)", g_passed);
    }

    std::println("\n──── test_dead_coercion_dirty_cone ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dead_coercion_dirty_cone() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dead_coercion_dirty_cone ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dead_coercion_dirty_cone ({} checks)", g_passed);
    }

    std::println("\n──── test_dead_coercion_layered ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dead_coercion_layered() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dead_coercion_layered ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dead_coercion_layered ({} checks)", g_passed);
    }

    std::println("\n──── test_instance_constraint_depth_cap ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_instance_constraint_depth_cap() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_instance_constraint_depth_cap ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_instance_constraint_depth_cap ({} checks)", g_passed);
    }

    std::println("\n──── test_occurrence_cache_key ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_occurrence_cache_key() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_occurrence_cache_key ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_occurrence_cache_key ({} checks)", g_passed);
    }

    std::println("\n──── test_occurrence_dirty_key_authority ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_occurrence_dirty_key_authority() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_occurrence_dirty_key_authority ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_occurrence_dirty_key_authority ({} checks)", g_passed);
    }

    std::println("\n──── test_occurrence_goal_epoch_table ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_occurrence_goal_epoch_table() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_occurrence_goal_epoch_table ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_occurrence_goal_epoch_table ({} checks)", g_passed);
    }

    std::println("\n──── test_occurrence_goal_persist_rehydrate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_occurrence_goal_persist_rehydrate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_occurrence_goal_persist_rehydrate ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_occurrence_goal_persist_rehydrate ({} checks)", g_passed);
    }

    std::println("\n──── test_occurrence_goal_vacuous_solve_prevent ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_occurrence_goal_vacuous_solve_prevent() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_occurrence_goal_vacuous_solve_prevent ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_occurrence_goal_vacuous_solve_prevent ({} checks)", g_passed);
    }

    std::println("\n──── test_partial_cone_cap ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_partial_cone_cap() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_partial_cone_cap ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_partial_cone_cap ({} checks)", g_passed);
    }

    std::println("\n──── test_partial_cone_commit_gate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_partial_cone_commit_gate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_partial_cone_commit_gate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_partial_cone_commit_gate ({} checks)", g_passed);
    }

    std::println("\n──── test_solve_delta_unresolved_export ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_solve_delta_unresolved_export() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_solve_delta_unresolved_export ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_solve_delta_unresolved_export ({} checks)", g_passed);
    }

    std::println("\n──── test_type_dep_epoch_prune ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_dep_epoch_prune() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_dep_epoch_prune ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_dep_epoch_prune ({} checks)", g_passed);
    }

    std::println("\n──── test_type_dep_partial_merge ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_dep_partial_merge() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_dep_partial_merge ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_dep_partial_merge ({} checks)", g_passed);
    }

    std::println("\n──── test_type_dirty_cone_dep_graph ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_dirty_cone_dep_graph() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_dirty_cone_dep_graph ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_dirty_cone_dep_graph ({} checks)", g_passed);
    }

    std::println("\n──── test_type_linear_commit_health ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_linear_commit_health() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_linear_commit_health ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_linear_commit_health ({} checks)", g_passed);
    }

    std::println("\n──── test_type_system_health ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_system_health() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_system_health ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_system_health ({} checks)", g_passed);
    }

    std::println("\n──── test_type_system_health_next_action ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_system_health_next_action() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_system_health_next_action ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_system_health_next_action ({} checks)", g_passed);
    }

    std::println("\n──── test_type_timeout_repair ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_timeout_repair() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_timeout_repair ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_timeout_repair ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
