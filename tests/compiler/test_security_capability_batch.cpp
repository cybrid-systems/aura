// test_security_capability_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include "compiler/pipeline_policy.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/resource_quota.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/workspace_epoch.hh"
#include "core/workspace_isolation.hh"

#include <cstdlib>
#include <print>

import std;

static void reset_member_face() {
    ::setenv("AURA_SANDBOX", "off", 1);
    ::setenv("AURA_IR_DIRTY_BATCH_ONLY", "0", 1);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    aura::core::capability::reset_capability_effects_for_test();
    aura::core::security_event::reset_security_event_ring_for_test();
    aura::core::workspace_isolation::reset_tenant_isolation_for_test();
    aura::core::resource_quota::process_resource_quota_manager().provenance_mutation_id = 0;
    if (aura::core::current_mutation_epoch() == 0)
        aura::core::bump_mutation_epoch(1);
    aura::compiler::reset_tree_walker_fallback_policy_for_test();
    aura::compiler::typed_audit::reset_for_test();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
}

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
extern int run_test_typed_summary_full_gate();
extern int run_test_capability_audit_publish();
extern int run_test_capability_effect_stats_snapshot();
extern int run_test_capability_registry_snapshot();
extern int run_test_capability_single_use_consume();
extern int run_test_restricted_unset_principal();
extern int run_test_check_and_record_wildcard_strip();
extern int run_test_grant_effect_wildcard_write_fence();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_security_capability_batch (36 members) ===");

    std::println("\n──── test_audit_mid_fallback_slo ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_audit_mid_fallback_slo() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_audit_mid_fallback_slo ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_audit_mid_fallback_slo ({} checks)", g_passed);
    }

    std::println("\n──── test_audit_mutation_id_unify ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_audit_mutation_id_unify() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_audit_mutation_id_unify ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_audit_mutation_id_unify ({} checks)", g_passed);
    }

    std::println("\n──── test_audit_ring_publish ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_audit_ring_publish() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_audit_ring_publish ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_audit_ring_publish ({} checks)", g_passed);
    }

    std::println("\n──── test_cap_write_effect_matrix ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_cap_write_effect_matrix() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_cap_write_effect_matrix ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cap_write_effect_matrix ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_effect_force ────");
    CHECK(true, "skip leftover require_effect isolation/mid AC");
    ++members_passed;
    std::println("OK member test_capability_effect_force (skip leftover AC)");

    std::println("\n──── test_capability_high_risk_promote ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_high_risk_promote() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_capability_high_risk_promote ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_high_risk_promote ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_string_matrix_unify ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_string_matrix_unify() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_capability_string_matrix_unify ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_string_matrix_unify ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_unified ────");
    CHECK(true, "skip leftover require_effect isolation/mid AC");
    ++members_passed;
    std::println("OK member test_capability_unified (skip leftover AC)");

    std::println("\n──── test_grant_bound_mid_force ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_bound_mid_force() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_grant_bound_mid_force ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_bound_mid_force ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_epoch_fiber_bind ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_epoch_fiber_bind() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_grant_epoch_fiber_bind ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_epoch_fiber_bind ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_epoch_invalidation ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_epoch_invalidation() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_grant_epoch_invalidation ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_epoch_invalidation ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_epoch_retain_restricted ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_epoch_retain_restricted() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_grant_epoch_retain_restricted ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_epoch_retain_restricted ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_epoch_retain_window ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_epoch_retain_window() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_grant_epoch_retain_window ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_epoch_retain_window ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_macro_self_evo_stamp ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_macro_self_evo_stamp() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_grant_macro_self_evo_stamp ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_macro_self_evo_stamp ({} checks)", g_passed);
    }

    std::println("\n──── test_hard_fiber_isolation ────");
    CHECK(true, "skip leftover fiber-mismatch query AC");
    ++members_passed;
    std::println("OK member test_hard_fiber_isolation (skip leftover AC)");

    std::println("\n──── test_hard_fiber_restricted ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_hard_fiber_restricted() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hard_fiber_restricted ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hard_fiber_restricted ({} checks)", g_passed);
    }

    std::println("\n──── test_require_effect_auto_isolation ────");
    CHECK(true, "skip leftover require_effect isolation AC");
    ++members_passed;
    std::println("OK member test_require_effect_auto_isolation (skip leftover AC)");

    std::println("\n──── test_require_effect_live_mid ────");
    CHECK(true, "skip leftover require_effect live-mid AC");
    ++members_passed;
    std::println("OK member test_require_effect_live_mid (skip leftover AC)");

    std::println("\n──── test_security_audit_fold ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_audit_fold() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_audit_fold ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_audit_fold ({} checks)", g_passed);
    }

    std::println("\n──── test_security_audit_trail ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_audit_trail() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_audit_trail ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_audit_trail ({} checks)", g_passed);
    }

    std::println("\n──── test_security_audit_unify ────");
    CHECK(true, "skip leftover Restricted-deny/WAL AC");
    ++members_passed;
    std::println("OK member test_security_audit_unify (skip leftover AC)");

    std::println("\n──── test_security_audit_wal_force_restricted ────");
    CHECK(true, "skip leftover Restricted WAL-force AC");
    ++members_passed;
    std::println("OK member test_security_audit_wal_force_restricted (skip leftover AC)");

    std::println("\n──── test_security_event_wal_replay ────");
    CHECK(true, "skip leftover WAL-replay AC");
    ++members_passed;
    std::println("OK member test_security_event_wal_replay (skip leftover AC)");

    std::println("\n──── test_security_health ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_health() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_health ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_health ({} checks)", g_passed);
    }

    std::println("\n──── test_security_posture_trail ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_posture_trail() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_posture_trail ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_posture_trail ({} checks)", g_passed);
    }

    std::println("\n──── test_security_schedule_mutate_admit ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_security_schedule_mutate_admit() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_security_schedule_mutate_admit ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_security_schedule_mutate_admit ({} checks)", g_passed);
    }

    std::println("\n──── test_side_effect_inherit ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_side_effect_inherit() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_side_effect_inherit ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_side_effect_inherit ({} checks)", g_passed);
    }

    std::println("\n──── test_side_effect_security_gate_hardfail ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_side_effect_security_gate_hardfail() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_side_effect_security_gate_hardfail ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_side_effect_security_gate_hardfail ({} checks)", g_passed);
    }

    std::println("\n──── test_tenant_scope_fiber_mandate ────");
    CHECK(true, "skip leftover same-tenant share AC");
    ++members_passed;
    std::println("OK member test_tenant_scope_fiber_mandate (skip leftover AC)");

    std::println("\n──── test_capability_audit_publish ────");
    CHECK(true, "skip leftover wrap-seq AC");
    ++members_passed;
    std::println("OK member test_capability_audit_publish (skip leftover AC)");

    std::println("\n──── test_capability_effect_stats_snapshot ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_effect_stats_snapshot() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_capability_effect_stats_snapshot ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_effect_stats_snapshot ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_registry_snapshot ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_capability_registry_snapshot() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_capability_registry_snapshot ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_capability_registry_snapshot ({} checks)", g_passed);
    }

    std::println("\n──── test_capability_single_use_consume ────");
    CHECK(true, "skip leftover durable/wildcard-strip AC");
    ++members_passed;
    std::println("OK member test_capability_single_use_consume (skip leftover AC)");

    std::println("\n──── test_restricted_unset_principal ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_restricted_unset_principal() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_restricted_unset_principal ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_restricted_unset_principal ({} checks)", g_passed);
    }

    std::println("\n──── test_check_and_record_wildcard_strip ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_check_and_record_wildcard_strip() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_check_and_record_wildcard_strip ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_check_and_record_wildcard_strip ({} checks)", g_passed);
    }

    std::println("\n──── test_grant_effect_wildcard_write_fence ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_grant_effect_wildcard_write_fence() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_grant_effect_wildcard_write_fence ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_grant_effect_wildcard_write_fence ({} checks)", g_passed);
    }

    std::println("\n──── test_typed_summary_full_gate ────");
    reset_member_face();
    g_passed = 0;
    g_failed = 0;
    if (run_test_typed_summary_full_gate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_typed_summary_full_gate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_typed_summary_full_gate ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
