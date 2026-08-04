// test_security_capability_batch.cpp — thematic multi-TU batch
// Security / capability / grant / Restricted / audit
// Members export run_<name>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.
// Policy: tests/HOMES.md — extend this batch, do not add test_*_<issue>.cpp.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_audit_mid_fallback_slo_2594();
extern int run_test_audit_mutation_id_unify_2493();
extern int run_test_audit_ring_publish_2530();
extern int run_test_cap_write_effect_matrix_2532();
extern int run_test_capability_effect_force_2072();
extern int run_test_capability_high_risk_promote_2489();
extern int run_test_capability_string_matrix_unify_2387();
extern int run_test_capability_unified_2077();
extern int run_test_grant_bound_mid_force_2531();
extern int run_test_grant_epoch_fiber_bind_2055();
extern int run_test_grant_epoch_invalidation_2074();
extern int run_test_grant_epoch_retain_restricted_2529();
extern int run_test_grant_epoch_retain_window_2154();
extern int run_test_grant_macro_self_evo_stamp_2386();
extern int run_test_hard_fiber_isolation_2151();
extern int run_test_hard_fiber_restricted_2536();
extern int run_test_require_effect_auto_isolation_2490();
extern int run_test_require_effect_live_mid_2384();
extern int run_test_security_audit_fold_2388();
extern int run_test_security_audit_trail_2075();
extern int run_test_security_audit_unify_2054();
extern int run_test_security_audit_wal_force_restricted_2492();
extern int run_test_security_event_wal_replay_2225();
extern int run_test_security_health_2389();
extern int run_test_security_posture_trail_2534();
extern int run_test_security_schedule_mutate_admit_2630();
extern int run_test_side_effect_inherit_2057();
extern int run_test_side_effect_security_gate_hardfail_2494();
extern int run_test_tenant_scope_fiber_mandate_2491();
extern int run_test_capability_audit_publish_2425();
extern int run_test_capability_effect_stats_snapshot_2430();
extern int run_test_capability_registry_snapshot_2426();
extern int run_test_capability_single_use_consume_2586();
extern int run_test_restricted_unset_principal_2385();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_security_capability_batch (34 members) ===");

    std::println("\n──── test_audit_mid_fallback_slo_2594 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_audit_mid_fallback_slo_2594() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_audit_mid_fallback_slo_2594 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_audit_mid_fallback_slo_2594 ({} checks)", g_passed);
    }

    std::println("\n──── test_audit_mutation_id_unify_2493 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_audit_mutation_id_unify_2493() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_audit_mutation_id_unify_2493 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_audit_mutation_id_unify_2493 ({} checks)", g_passed);
    }

    std::println("\n──── test_audit_ring_publish_2530 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_audit_ring_publish_2530() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_audit_ring_publish_2530 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_audit_ring_publish_2530 ({} checks)", g_passed);
    }

    std::println("\n──── test_cap_write_effect_matrix_2532 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_cap_write_effect_matrix_2532() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_cap_write_effect_matrix_2532 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cap_write_effect_matrix_2532 ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_effect_force_2072 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_effect_force_2072() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_capability_effect_force_2072 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_effect_force_2072 ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_high_risk_promote_2489 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_high_risk_promote_2489() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_capability_high_risk_promote_2489 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_high_risk_promote_2489 ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_string_matrix_unify_2387 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_string_matrix_unify_2387() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_capability_string_matrix_unify_2387 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_string_matrix_unify_2387 ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_unified_2077 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_unified_2077() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_capability_unified_2077 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_unified_2077 ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_bound_mid_force_2531 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_bound_mid_force_2531() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_grant_bound_mid_force_2531 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_bound_mid_force_2531 ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_epoch_fiber_bind_2055 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_epoch_fiber_bind_2055() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_grant_epoch_fiber_bind_2055 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_epoch_fiber_bind_2055 ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_epoch_invalidation_2074 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_epoch_invalidation_2074() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_grant_epoch_invalidation_2074 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_epoch_invalidation_2074 ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_epoch_retain_restricted_2529 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_epoch_retain_restricted_2529() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_grant_epoch_retain_restricted_2529 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_epoch_retain_restricted_2529 ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_epoch_retain_window_2154 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_epoch_retain_window_2154() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_grant_epoch_retain_window_2154 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_epoch_retain_window_2154 ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_macro_self_evo_stamp_2386 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_macro_self_evo_stamp_2386() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_grant_macro_self_evo_stamp_2386 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_macro_self_evo_stamp_2386 ({} checks)", g_passed);
    }

    std::println("\n──── test_hard_fiber_isolation_2151 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hard_fiber_isolation_2151() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hard_fiber_isolation_2151 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hard_fiber_isolation_2151 ({} checks)", g_passed);
    }

    std::println("\n──── test_hard_fiber_restricted_2536 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hard_fiber_restricted_2536() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hard_fiber_restricted_2536 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hard_fiber_restricted_2536 ({} checks)", g_passed);
    }

    std::println("\n──── test_require_effect_auto_isolation_2490 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_require_effect_auto_isolation_2490() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_require_effect_auto_isolation_2490 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_require_effect_auto_isolation_2490 ({} checks)", g_passed);
    }

    std::println("\n──── test_require_effect_live_mid_2384 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_require_effect_live_mid_2384() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_require_effect_live_mid_2384 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_require_effect_live_mid_2384 ({} checks)", g_passed);
    }

    std::println("\n──── test_security_audit_fold_2388 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_audit_fold_2388() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_audit_fold_2388 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_audit_fold_2388 ({} checks)", g_passed);
    }

    std::println("\n──── test_security_audit_trail_2075 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_audit_trail_2075() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_audit_trail_2075 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_audit_trail_2075 ({} checks)", g_passed);
    }

    std::println("\n──── test_security_audit_unify_2054 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_audit_unify_2054() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_audit_unify_2054 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_audit_unify_2054 ({} checks)", g_passed);
    }

    std::println("\n──── test_security_audit_wal_force_restricted_2492 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_audit_wal_force_restricted_2492() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_audit_wal_force_restricted_2492 (checks: {} "
                     "passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_audit_wal_force_restricted_2492 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_security_event_wal_replay_2225 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_event_wal_replay_2225() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_security_event_wal_replay_2225 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_event_wal_replay_2225 ({} checks)", g_passed);
    }

    std::println("\n──── test_security_health_2389 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_health_2389() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_health_2389 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_health_2389 ({} checks)", g_passed);
    }

    std::println("\n──── test_security_posture_trail_2534 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_posture_trail_2534() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_posture_trail_2534 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_posture_trail_2534 ({} checks)", g_passed);
    }

    std::println("\n──── test_security_schedule_mutate_admit_2630 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_schedule_mutate_admit_2630() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_security_schedule_mutate_admit_2630 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_schedule_mutate_admit_2630 ({} checks)", g_passed);
    }

    std::println("\n──── test_side_effect_inherit_2057 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_side_effect_inherit_2057() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_side_effect_inherit_2057 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_side_effect_inherit_2057 ({} checks)", g_passed);
    }

    std::println("\n──── test_side_effect_security_gate_hardfail_2494 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_side_effect_security_gate_hardfail_2494() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_side_effect_security_gate_hardfail_2494 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_side_effect_security_gate_hardfail_2494 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_tenant_scope_fiber_mandate_2491 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_tenant_scope_fiber_mandate_2491() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_tenant_scope_fiber_mandate_2491 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_tenant_scope_fiber_mandate_2491 ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_audit_publish_2425 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_audit_publish_2425() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_capability_audit_publish_2425 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_audit_publish_2425 ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_effect_stats_snapshot_2430 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_effect_stats_snapshot_2430() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_capability_effect_stats_snapshot_2430 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_effect_stats_snapshot_2430 ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_registry_snapshot_2426 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_registry_snapshot_2426() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_capability_registry_snapshot_2426 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_registry_snapshot_2426 ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_single_use_consume_2586 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_single_use_consume_2586() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_capability_single_use_consume_2586 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_single_use_consume_2586 ({} checks)", g_passed);
    }

    std::println("\n──── test_restricted_unset_principal_2385 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_restricted_unset_principal_2385() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_restricted_unset_principal_2385 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_restricted_unset_principal_2385 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
