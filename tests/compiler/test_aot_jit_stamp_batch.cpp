// test_aot_jit_stamp_batch.cpp — thematic multi-TU batch
// AOT / SpecJIT / stamp / relower / reload
// Members export run_<name>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.
// Policy: tests/HOMES.md — extend this batch, do not add test_*_<issue>.cpp.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_adaptive_partial_relower_threshold_2112();
extern int run_test_aot_anonymous_closure_policy_2238();
extern int run_test_aot_hot_update_health_2506();
extern int run_test_aot_jit_joint_versioning_2046();
extern int run_test_aot_version_triple_2306();
extern int run_test_cache_stamp_restamp_contract_2183();
extern int run_test_closure_cow_gen_stamp_2547();
extern int run_test_coercion_stamp_at_add_2512();
extern int run_test_exhausted_min_dirty_reemit_2544();
extern int run_test_instr_level_relower_pass_2133();
extern int run_test_ir_soa_layout_stamp_2432();
extern int run_test_isolation_stamp_resolve_2224();
extern int run_test_layout_stamp_2170();
extern int run_test_layout_stamp_equality_8field_2519();
extern int run_test_linear_state_stamp_apply_2129();
extern int run_test_live_closure_full_restamp_2542();
extern int run_test_partial_relower_cascade_2041();
extern int run_test_partial_relower_storm_gate_2190();
extern int run_test_pereval_reemit_region_independence_2606();
extern int run_test_reemit_mutation_boundary_handshake_2114();
extern int run_test_reemit_production_default_defer_2205();
extern int run_test_reemit_production_default_defer_2208();
extern int run_test_reload_recovery_query_2367();
extern int run_test_relower_fallback_reason_2193();
extern int run_test_shape_storm_partial_relower_2212();
extern int run_test_specjit_per_eval_storm_isolation_2370();
extern int run_test_specjit_pereval_storm_e2e_2504();
extern int run_test_workload_adaptive_relower_2127();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_aot_jit_stamp_batch (28 members) ===");

    std::println("\n──── test_adaptive_partial_relower_threshold_2112 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_adaptive_partial_relower_threshold_2112() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_adaptive_partial_relower_threshold_2112 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_adaptive_partial_relower_threshold_2112 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_aot_anonymous_closure_policy_2238 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_aot_anonymous_closure_policy_2238() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_aot_anonymous_closure_policy_2238 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_aot_anonymous_closure_policy_2238 ({} checks)", g_passed);
    }

    std::println("\n──── test_aot_hot_update_health_2506 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_aot_hot_update_health_2506() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_aot_hot_update_health_2506 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_aot_hot_update_health_2506 ({} checks)", g_passed);
    }

    std::println("\n──── test_aot_jit_joint_versioning_2046 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_aot_jit_joint_versioning_2046() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_aot_jit_joint_versioning_2046 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_aot_jit_joint_versioning_2046 ({} checks)", g_passed);
    }

    std::println("\n──── test_aot_version_triple_2306 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_aot_version_triple_2306() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_aot_version_triple_2306 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_aot_version_triple_2306 ({} checks)", g_passed);
    }

    std::println("\n──── test_cache_stamp_restamp_contract_2183 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_cache_stamp_restamp_contract_2183() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_cache_stamp_restamp_contract_2183 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cache_stamp_restamp_contract_2183 ({} checks)", g_passed);
    }

    std::println("\n──── test_closure_cow_gen_stamp_2547 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_closure_cow_gen_stamp_2547() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_closure_cow_gen_stamp_2547 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_closure_cow_gen_stamp_2547 ({} checks)", g_passed);
    }

    std::println("\n──── test_coercion_stamp_at_add_2512 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_coercion_stamp_at_add_2512() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_coercion_stamp_at_add_2512 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_coercion_stamp_at_add_2512 ({} checks)", g_passed);
    }

    std::println("\n──── test_exhausted_min_dirty_reemit_2544 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_exhausted_min_dirty_reemit_2544() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_exhausted_min_dirty_reemit_2544 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_exhausted_min_dirty_reemit_2544 ({} checks)", g_passed);
    }

    std::println("\n──── test_instr_level_relower_pass_2133 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_instr_level_relower_pass_2133() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_instr_level_relower_pass_2133 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_instr_level_relower_pass_2133 ({} checks)", g_passed);
    }

    std::println("\n──── test_ir_soa_layout_stamp_2432 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_ir_soa_layout_stamp_2432() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_ir_soa_layout_stamp_2432 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_ir_soa_layout_stamp_2432 ({} checks)", g_passed);
    }

    std::println("\n──── test_isolation_stamp_resolve_2224 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_isolation_stamp_resolve_2224() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_isolation_stamp_resolve_2224 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_isolation_stamp_resolve_2224 ({} checks)", g_passed);
    }

    std::println("\n──── test_layout_stamp_2170 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_layout_stamp_2170() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_layout_stamp_2170 (checks: {} passed, {} failed)", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_layout_stamp_2170 ({} checks)", g_passed);
    }

    std::println("\n──── test_layout_stamp_equality_8field_2519 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_layout_stamp_equality_8field_2519() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_layout_stamp_equality_8field_2519 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_layout_stamp_equality_8field_2519 ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_state_stamp_apply_2129 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_state_stamp_apply_2129() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_linear_state_stamp_apply_2129 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_state_stamp_apply_2129 ({} checks)", g_passed);
    }

    std::println("\n──── test_live_closure_full_restamp_2542 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_live_closure_full_restamp_2542() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_live_closure_full_restamp_2542 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_live_closure_full_restamp_2542 ({} checks)", g_passed);
    }

    std::println("\n──── test_partial_relower_cascade_2041 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_partial_relower_cascade_2041() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_partial_relower_cascade_2041 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_partial_relower_cascade_2041 ({} checks)", g_passed);
    }

    std::println("\n──── test_partial_relower_storm_gate_2190 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_partial_relower_storm_gate_2190() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_partial_relower_storm_gate_2190 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_partial_relower_storm_gate_2190 ({} checks)", g_passed);
    }

    std::println("\n──── test_pereval_reemit_region_independence_2606 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_pereval_reemit_region_independence_2606() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_pereval_reemit_region_independence_2606 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_pereval_reemit_region_independence_2606 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_reemit_mutation_boundary_handshake_2114 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reemit_mutation_boundary_handshake_2114() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_reemit_mutation_boundary_handshake_2114 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_reemit_mutation_boundary_handshake_2114 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_reemit_production_default_defer_2205 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reemit_production_default_defer_2205() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_reemit_production_default_defer_2205 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_reemit_production_default_defer_2205 ({} checks)", g_passed);
    }

    std::println("\n──── test_reemit_production_default_defer_2208 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reemit_production_default_defer_2208() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_reemit_production_default_defer_2208 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_reemit_production_default_defer_2208 ({} checks)", g_passed);
    }

    std::println("\n──── test_reload_recovery_query_2367 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reload_recovery_query_2367() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_reload_recovery_query_2367 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_reload_recovery_query_2367 ({} checks)", g_passed);
    }

    std::println("\n──── test_relower_fallback_reason_2193 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_relower_fallback_reason_2193() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_relower_fallback_reason_2193 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_relower_fallback_reason_2193 ({} checks)", g_passed);
    }

    std::println("\n──── test_shape_storm_partial_relower_2212 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_shape_storm_partial_relower_2212() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_shape_storm_partial_relower_2212 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_shape_storm_partial_relower_2212 ({} checks)", g_passed);
    }

    std::println("\n──── test_specjit_per_eval_storm_isolation_2370 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_specjit_per_eval_storm_isolation_2370() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_specjit_per_eval_storm_isolation_2370 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_specjit_per_eval_storm_isolation_2370 ({} checks)", g_passed);
    }

    std::println("\n──── test_specjit_pereval_storm_e2e_2504 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_specjit_pereval_storm_e2e_2504() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_specjit_pereval_storm_e2e_2504 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_specjit_pereval_storm_e2e_2504 ({} checks)", g_passed);
    }

    std::println("\n──── test_workload_adaptive_relower_2127 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_workload_adaptive_relower_2127() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_workload_adaptive_relower_2127 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_workload_adaptive_relower_2127 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
