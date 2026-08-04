// test_security_capability_batch.cpp — thematic multi-TU batch
// test_security_capability_batch (S3 renamed members)
// Stream S3: member filenames stripped of _NNNN issue suffixes.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_audit_mid_fallback_slo();
extern int run_test_audit_mutation_id_unify();
extern int run_test_audit_ring_publish();
extern int run_test_cap_write_effect_matrix();
extern int run_test_capability_effect_force();
extern int run_test_capability_high_risk_promote();
extern int run_test_capability_string_matrix_unify();
extern int run_test_capability_unified();
extern int run_test_grant_bound_mid_force();
extern int run_test_grant_epoch_fiber_bind();
extern int run_test_grant_epoch_invalidation();
extern int run_test_grant_epoch_retain_restricted();
extern int run_test_grant_epoch_retain_window();
extern int run_test_grant_macro_self_evo_stamp();
extern int run_test_hard_fiber_isolation();
extern int run_test_hard_fiber_restricted();
extern int run_test_require_effect_auto_isolation();
extern int run_test_require_effect_live_mid();
extern int run_test_security_audit_fold();
extern int run_test_security_audit_trail();
extern int run_test_security_audit_unify();
extern int run_test_security_audit_wal_force_restricted();
extern int run_test_security_event_wal_replay();
extern int run_test_security_health();
extern int run_test_security_posture_trail();
extern int run_test_security_schedule_mutate_admit();
extern int run_test_side_effect_inherit();
extern int run_test_side_effect_security_gate_hardfail();
extern int run_test_tenant_scope_fiber_mandate();
extern int run_test_capability_audit_publish();
extern int run_test_capability_effect_stats_snapshot();
extern int run_test_capability_registry_snapshot();
extern int run_test_capability_single_use_consume();
extern int run_test_restricted_unset_principal();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_security_capability_batch (34 members) ===");

    std::println("\n──── test_audit_mid_fallback_slo ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_audit_mid_fallback_slo() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_audit_mid_fallback_slo (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_audit_mid_fallback_slo ({} checks)", g_passed);
    }

    std::println("\n──── test_audit_mutation_id_unify ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_audit_mutation_id_unify() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_audit_mutation_id_unify (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_audit_mutation_id_unify ({} checks)", g_passed);
    }

    std::println("\n──── test_audit_ring_publish ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_audit_ring_publish() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_audit_ring_publish (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_audit_ring_publish ({} checks)", g_passed);
    }

    std::println("\n──── test_cap_write_effect_matrix ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_cap_write_effect_matrix() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_cap_write_effect_matrix (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cap_write_effect_matrix ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_effect_force ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_effect_force() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_capability_effect_force (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_effect_force ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_high_risk_promote ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_high_risk_promote() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_capability_high_risk_promote (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_high_risk_promote ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_string_matrix_unify ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_string_matrix_unify() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_capability_string_matrix_unify (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_string_matrix_unify ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_unified ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_unified() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_capability_unified (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_unified ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_bound_mid_force ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_bound_mid_force() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_grant_bound_mid_force (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_bound_mid_force ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_epoch_fiber_bind ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_epoch_fiber_bind() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_grant_epoch_fiber_bind (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_epoch_fiber_bind ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_epoch_invalidation ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_epoch_invalidation() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_grant_epoch_invalidation (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_epoch_invalidation ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_epoch_retain_restricted ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_epoch_retain_restricted() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_grant_epoch_retain_restricted (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_epoch_retain_restricted ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_epoch_retain_window ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_epoch_retain_window() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_grant_epoch_retain_window (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_epoch_retain_window ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_macro_self_evo_stamp ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_macro_self_evo_stamp() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_grant_macro_self_evo_stamp (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_macro_self_evo_stamp ({} checks)", g_passed);
    }

    std::println("\n──── test_hard_fiber_isolation ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hard_fiber_isolation() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hard_fiber_isolation (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hard_fiber_isolation ({} checks)", g_passed);
    }

    std::println("\n──── test_hard_fiber_restricted ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hard_fiber_restricted() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hard_fiber_restricted (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hard_fiber_restricted ({} checks)", g_passed);
    }

    std::println("\n──── test_require_effect_auto_isolation ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_require_effect_auto_isolation() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_require_effect_auto_isolation (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_require_effect_auto_isolation ({} checks)", g_passed);
    }

    std::println("\n──── test_require_effect_live_mid ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_require_effect_live_mid() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_require_effect_live_mid (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_require_effect_live_mid ({} checks)", g_passed);
    }

    std::println("\n──── test_security_audit_fold ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_audit_fold() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_audit_fold (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_audit_fold ({} checks)", g_passed);
    }

    std::println("\n──── test_security_audit_trail ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_audit_trail() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_audit_trail (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_audit_trail ({} checks)", g_passed);
    }

    std::println("\n──── test_security_audit_unify ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_audit_unify() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_audit_unify (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_audit_unify ({} checks)", g_passed);
    }

    std::println("\n──── test_security_audit_wal_force_restricted ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_audit_wal_force_restricted() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_security_audit_wal_force_restricted (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_audit_wal_force_restricted ({} checks)", g_passed);
    }

    std::println("\n──── test_security_event_wal_replay ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_event_wal_replay() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_event_wal_replay (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_event_wal_replay ({} checks)", g_passed);
    }

    std::println("\n──── test_security_health ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_health() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_health (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_health ({} checks)", g_passed);
    }

    std::println("\n──── test_security_posture_trail ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_posture_trail() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_posture_trail (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_posture_trail ({} checks)", g_passed);
    }

    std::println("\n──── test_security_schedule_mutate_admit ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_schedule_mutate_admit() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_security_schedule_mutate_admit (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_schedule_mutate_admit ({} checks)", g_passed);
    }

    std::println("\n──── test_side_effect_inherit ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_side_effect_inherit() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_side_effect_inherit (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_side_effect_inherit ({} checks)", g_passed);
    }

    std::println("\n──── test_side_effect_security_gate_hardfail ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_side_effect_security_gate_hardfail() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_side_effect_security_gate_hardfail (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_side_effect_security_gate_hardfail ({} checks)", g_passed);
    }

    std::println("\n──── test_tenant_scope_fiber_mandate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_tenant_scope_fiber_mandate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_tenant_scope_fiber_mandate (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_tenant_scope_fiber_mandate ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_audit_publish ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_audit_publish() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_capability_audit_publish (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_audit_publish ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_effect_stats_snapshot ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_effect_stats_snapshot() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_capability_effect_stats_snapshot (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_effect_stats_snapshot ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_registry_snapshot ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_registry_snapshot() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_capability_registry_snapshot (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_registry_snapshot ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_single_use_consume ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_single_use_consume() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_capability_single_use_consume (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_single_use_consume ({} checks)", g_passed);
    }

    std::println("\n──── test_restricted_unset_principal ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_restricted_unset_principal() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_restricted_unset_principal (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_restricted_unset_principal ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
