// test_aot_jit_stamp_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_adaptive_partial_relower_threshold();
extern int run_test_aot_anonymous_closure_policy();
extern int run_test_aot_hot_update_health();
extern int run_test_aot_jit_joint_versioning();
extern int run_test_aot_version_triple();
extern int run_test_cache_stamp_restamp_contract();
extern int run_test_closure_cow_gen_stamp();
extern int run_test_coercion_stamp_at_add();
extern int run_test_exhausted_min_dirty_reemit();
extern int run_test_instr_level_relower_pass();
extern int run_test_ir_soa_layout_stamp();
extern int run_test_isolation_stamp_resolve();
extern int run_test_layout_stamp();
extern int run_test_layout_stamp_equality_8field();
extern int run_test_linear_state_stamp_apply();
extern int run_test_live_closure_full_restamp();
extern int run_test_partial_relower_cascade();
extern int run_test_partial_relower_storm_gate();
extern int run_test_pereval_reemit_region_independence();
extern int run_test_reemit_mutation_boundary_handshake();
extern int run_test_reload_recovery_query();
extern int run_test_relower_fallback_reason();
extern int run_test_shape_storm_partial_relower();
extern int run_test_specjit_per_eval_storm_isolation();
extern int run_test_specjit_pereval_storm_e2e();
extern int run_test_workload_adaptive_relower();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_aot_jit_stamp_batch (26 members) ===");

    std::println("\n──── test_adaptive_partial_relower_threshold ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_adaptive_partial_relower_threshold() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_adaptive_partial_relower_threshold ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_adaptive_partial_relower_threshold ({} checks)", g_passed);
    }

    std::println("\n──── test_aot_anonymous_closure_policy ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_aot_anonymous_closure_policy() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_aot_anonymous_closure_policy ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_aot_anonymous_closure_policy ({} checks)", g_passed);
    }

    std::println("\n──── test_aot_hot_update_health ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_aot_hot_update_health() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_aot_hot_update_health ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_aot_hot_update_health ({} checks)", g_passed);
    }

    std::println("\n──── test_aot_jit_joint_versioning ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_aot_jit_joint_versioning() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_aot_jit_joint_versioning ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_aot_jit_joint_versioning ({} checks)", g_passed);
    }

    std::println("\n──── test_aot_version_triple ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_aot_version_triple() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_aot_version_triple ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_aot_version_triple ({} checks)", g_passed);
    }

    std::println("\n──── test_cache_stamp_restamp_contract ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_cache_stamp_restamp_contract() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_cache_stamp_restamp_contract ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cache_stamp_restamp_contract ({} checks)", g_passed);
    }

    std::println("\n──── test_closure_cow_gen_stamp ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_closure_cow_gen_stamp() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_closure_cow_gen_stamp ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_closure_cow_gen_stamp ({} checks)", g_passed);
    }

    std::println("\n──── test_coercion_stamp_at_add ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_stamp_at_add() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_stamp_at_add ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_stamp_at_add ({} checks)", g_passed);
    }

    std::println("\n──── test_exhausted_min_dirty_reemit ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_exhausted_min_dirty_reemit() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_exhausted_min_dirty_reemit ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_exhausted_min_dirty_reemit ({} checks)", g_passed);
    }

    std::println("\n──── test_instr_level_relower_pass ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_instr_level_relower_pass() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_instr_level_relower_pass ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_instr_level_relower_pass ({} checks)", g_passed);
    }

    std::println("\n──── test_ir_soa_layout_stamp ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_ir_soa_layout_stamp() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_ir_soa_layout_stamp ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_ir_soa_layout_stamp ({} checks)", g_passed);
    }

    std::println("\n──── test_isolation_stamp_resolve ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_isolation_stamp_resolve() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_isolation_stamp_resolve ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_isolation_stamp_resolve ({} checks)", g_passed);
    }

    std::println("\n──── test_layout_stamp ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_layout_stamp() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_layout_stamp ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_layout_stamp ({} checks)", g_passed);
    }

    std::println("\n──── test_layout_stamp_equality_8field ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_layout_stamp_equality_8field() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_layout_stamp_equality_8field ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_layout_stamp_equality_8field ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_state_stamp_apply ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_state_stamp_apply() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_state_stamp_apply ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_state_stamp_apply ({} checks)", g_passed);
    }

    std::println("\n──── test_live_closure_full_restamp ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_live_closure_full_restamp() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_live_closure_full_restamp ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_live_closure_full_restamp ({} checks)", g_passed);
    }

    std::println("\n──── test_partial_relower_cascade ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_partial_relower_cascade() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_partial_relower_cascade ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_partial_relower_cascade ({} checks)", g_passed);
    }

    std::println("\n──── test_partial_relower_storm_gate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_partial_relower_storm_gate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_partial_relower_storm_gate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_partial_relower_storm_gate ({} checks)", g_passed);
    }

    std::println("\n──── test_pereval_reemit_region_independence ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_pereval_reemit_region_independence() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_pereval_reemit_region_independence ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_pereval_reemit_region_independence ({} checks)", g_passed);
    }

    std::println("\n──── test_reemit_mutation_boundary_handshake ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reemit_mutation_boundary_handshake() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_reemit_mutation_boundary_handshake ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_reemit_mutation_boundary_handshake ({} checks)", g_passed);
    }

    std::println("\n──── test_reload_recovery_query ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reload_recovery_query() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_reload_recovery_query ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_reload_recovery_query ({} checks)", g_passed);
    }

    std::println("\n──── test_relower_fallback_reason ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_relower_fallback_reason() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_relower_fallback_reason ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_relower_fallback_reason ({} checks)", g_passed);
    }

    std::println("\n──── test_shape_storm_partial_relower ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_shape_storm_partial_relower() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_shape_storm_partial_relower ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_shape_storm_partial_relower ({} checks)", g_passed);
    }

    std::println("\n──── test_specjit_per_eval_storm_isolation ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_specjit_per_eval_storm_isolation() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_specjit_per_eval_storm_isolation ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_specjit_per_eval_storm_isolation ({} checks)", g_passed);
    }

    std::println("\n──── test_specjit_pereval_storm_e2e ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_specjit_pereval_storm_e2e() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_specjit_pereval_storm_e2e ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_specjit_pereval_storm_e2e ({} checks)", g_passed);
    }

    std::println("\n──── test_workload_adaptive_relower ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_workload_adaptive_relower() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_workload_adaptive_relower ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_workload_adaptive_relower ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
