// test_ir_closure_jit_misc_batch.cpp — thematic multi-TU batch
// IR / closure / JIT / deopt / DCE / PrimCall ACs
// Stream A3 of tests/CONSOLIDATION_PLAN.md — carved from misc_issue_fold.
// Note: tests/reflect/test_ir_pod_phase4_2291 stays outside (reflect/-freflection).
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_anonymous_residual_stable_id_policy_2605();
extern int run_test_aura_jit_unused_fn_lock_2475();
extern int run_test_closure_call_must_deopt_toctou_2472();
extern int run_test_comprehensive_live_closure_expire_2042();
extern int run_test_dce_elided_deopt_meta_2611();
extern int run_test_emit_object_deprecated_2477();
extern int run_test_force_jit_repromote_2502();
extern int run_test_ir_const_string_intern_2573();
extern int run_test_ir_optimize_type_info_chain_2471();
extern int run_test_jit_dual_string_heap_2575();
extern int run_test_jit_interpreter_equivalence_oracle_2210();
extern int run_test_jit_macro_deopt_hygiene_2100();
extern int run_test_live_closure_stable_id_only_2369();
extern int run_test_must_deopt_before_next_call_2128();
extern int run_test_named_closure_stable_id_at_create_2550();
extern int run_test_partial_recompile_single_evict_2476();
extern int run_test_primcall_narg_2576();
extern int run_test_primcall_str_intern_2577();
extern int run_test_region_priority_deopt_throttle_2132();
extern int run_test_remount_force_deopt_2503();
extern int run_test_source_to_ir_desync_recovery_2206();
extern int run_test_source_to_ir_map_consistency_2045();
extern int run_test_tree_walker_fallback_strict_2213();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0, members_passed = 0;
    std::println("=== test_ir_closure_jit_misc_batch (23 members) ===");

    std::println("\n──── test_anonymous_residual_stable_id_policy_2605 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_anonymous_residual_stable_id_policy_2605() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_anonymous_residual_stable_id_policy_2605 ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_anonymous_residual_stable_id_policy_2605 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_aura_jit_unused_fn_lock_2475 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_aura_jit_unused_fn_lock_2475() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_aura_jit_unused_fn_lock_2475 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_aura_jit_unused_fn_lock_2475 ({} checks)", g_passed);
    }

    std::println("\n──── test_closure_call_must_deopt_toctou_2472 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_closure_call_must_deopt_toctou_2472() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_closure_call_must_deopt_toctou_2472 ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_closure_call_must_deopt_toctou_2472 ({} checks)", g_passed);
    }

    std::println("\n──── test_comprehensive_live_closure_expire_2042 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_comprehensive_live_closure_expire_2042() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_comprehensive_live_closure_expire_2042 ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_comprehensive_live_closure_expire_2042 ({} checks)", g_passed);
    }

    std::println("\n──── test_dce_elided_deopt_meta_2611 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dce_elided_deopt_meta_2611() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dce_elided_deopt_meta_2611 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dce_elided_deopt_meta_2611 ({} checks)", g_passed);
    }

    std::println("\n──── test_emit_object_deprecated_2477 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_emit_object_deprecated_2477() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_emit_object_deprecated_2477 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_emit_object_deprecated_2477 ({} checks)", g_passed);
    }

    std::println("\n──── test_force_jit_repromote_2502 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_force_jit_repromote_2502() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_force_jit_repromote_2502 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_force_jit_repromote_2502 ({} checks)", g_passed);
    }

    std::println("\n──── test_ir_const_string_intern_2573 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_ir_const_string_intern_2573() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_ir_const_string_intern_2573 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_ir_const_string_intern_2573 ({} checks)", g_passed);
    }

    std::println("\n──── test_ir_optimize_type_info_chain_2471 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_ir_optimize_type_info_chain_2471() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_ir_optimize_type_info_chain_2471 ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_ir_optimize_type_info_chain_2471 ({} checks)", g_passed);
    }

    std::println("\n──── test_jit_dual_string_heap_2575 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_jit_dual_string_heap_2575() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_jit_dual_string_heap_2575 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_jit_dual_string_heap_2575 ({} checks)", g_passed);
    }

    std::println("\n──── test_jit_interpreter_equivalence_oracle_2210 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_jit_interpreter_equivalence_oracle_2210() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_jit_interpreter_equivalence_oracle_2210 ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_jit_interpreter_equivalence_oracle_2210 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_jit_macro_deopt_hygiene_2100 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_jit_macro_deopt_hygiene_2100() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_jit_macro_deopt_hygiene_2100 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_jit_macro_deopt_hygiene_2100 ({} checks)", g_passed);
    }

    std::println("\n──── test_live_closure_stable_id_only_2369 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_live_closure_stable_id_only_2369() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_live_closure_stable_id_only_2369 ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_live_closure_stable_id_only_2369 ({} checks)", g_passed);
    }

    std::println("\n──── test_must_deopt_before_next_call_2128 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_must_deopt_before_next_call_2128() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_must_deopt_before_next_call_2128 ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_must_deopt_before_next_call_2128 ({} checks)", g_passed);
    }

    std::println("\n──── test_named_closure_stable_id_at_create_2550 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_named_closure_stable_id_at_create_2550() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_named_closure_stable_id_at_create_2550 ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_named_closure_stable_id_at_create_2550 ({} checks)", g_passed);
    }

    std::println("\n──── test_partial_recompile_single_evict_2476 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_partial_recompile_single_evict_2476() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_partial_recompile_single_evict_2476 ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_partial_recompile_single_evict_2476 ({} checks)", g_passed);
    }

    std::println("\n──── test_primcall_narg_2576 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_primcall_narg_2576() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_primcall_narg_2576 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_primcall_narg_2576 ({} checks)", g_passed);
    }

    std::println("\n──── test_primcall_str_intern_2577 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_primcall_str_intern_2577() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_primcall_str_intern_2577 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_primcall_str_intern_2577 ({} checks)", g_passed);
    }

    std::println("\n──── test_region_priority_deopt_throttle_2132 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_region_priority_deopt_throttle_2132() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_region_priority_deopt_throttle_2132 ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_region_priority_deopt_throttle_2132 ({} checks)", g_passed);
    }

    std::println("\n──── test_remount_force_deopt_2503 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_remount_force_deopt_2503() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_remount_force_deopt_2503 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_remount_force_deopt_2503 ({} checks)", g_passed);
    }

    std::println("\n──── test_source_to_ir_desync_recovery_2206 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_source_to_ir_desync_recovery_2206() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_source_to_ir_desync_recovery_2206 ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_source_to_ir_desync_recovery_2206 ({} checks)", g_passed);
    }

    std::println("\n──── test_source_to_ir_map_consistency_2045 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_source_to_ir_map_consistency_2045() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_source_to_ir_map_consistency_2045 ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_source_to_ir_map_consistency_2045 ({} checks)", g_passed);
    }

    std::println("\n──── test_tree_walker_fallback_strict_2213 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_tree_walker_fallback_strict_2213() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_tree_walker_fallback_strict_2213 ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_tree_walker_fallback_strict_2213 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
