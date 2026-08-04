// test_ir_closure_jit_misc_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_anonymous_residual_stable_id_policy();
extern int run_test_aura_jit_unused_fn_lock();
extern int run_test_closure_call_must_deopt_toctou();
extern int run_test_comprehensive_live_closure_expire();
extern int run_test_dce_elided_deopt_meta();
extern int run_test_emit_object_deprecated();
extern int run_test_force_jit_repromote();
extern int run_test_ir_const_string_intern();
extern int run_test_ir_optimize_type_info_chain();
extern int run_test_jit_dual_string_heap();
extern int run_test_jit_interpreter_equivalence_oracle();
extern int run_test_jit_macro_deopt_hygiene();
extern int run_test_live_closure_stable_id_only();
extern int run_test_must_deopt_before_next_call();
extern int run_test_named_closure_stable_id_at_create();
extern int run_test_partial_recompile_single_evict();
extern int run_test_primcall_narg();
extern int run_test_primcall_str_intern();
extern int run_test_region_priority_deopt_throttle();
extern int run_test_remount_force_deopt();
extern int run_test_source_to_ir_desync_recovery();
extern int run_test_source_to_ir_map_consistency();
extern int run_test_tree_walker_fallback_strict();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_ir_closure_jit_misc_batch (23 members) ===");

    std::println("\n──── test_anonymous_residual_stable_id_policy ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_anonymous_residual_stable_id_policy() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_anonymous_residual_stable_id_policy ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_anonymous_residual_stable_id_policy ({} checks)", g_passed);
    }

    std::println("\n──── test_aura_jit_unused_fn_lock ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_aura_jit_unused_fn_lock() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_aura_jit_unused_fn_lock ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_aura_jit_unused_fn_lock ({} checks)", g_passed);
    }

    std::println("\n──── test_closure_call_must_deopt_toctou ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_closure_call_must_deopt_toctou() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_closure_call_must_deopt_toctou ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_closure_call_must_deopt_toctou ({} checks)", g_passed);
    }

    std::println("\n──── test_comprehensive_live_closure_expire ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_comprehensive_live_closure_expire() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_comprehensive_live_closure_expire ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_comprehensive_live_closure_expire ({} checks)", g_passed);
    }

    std::println("\n──── test_dce_elided_deopt_meta ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_dce_elided_deopt_meta() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_dce_elided_deopt_meta ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_dce_elided_deopt_meta ({} checks)", g_passed);
    }

    std::println("\n──── test_emit_object_deprecated ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_emit_object_deprecated() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_emit_object_deprecated ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_emit_object_deprecated ({} checks)", g_passed);
    }

    std::println("\n──── test_force_jit_repromote ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_force_jit_repromote() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_force_jit_repromote ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_force_jit_repromote ({} checks)", g_passed);
    }

    std::println("\n──── test_ir_const_string_intern ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_ir_const_string_intern() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_ir_const_string_intern ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_ir_const_string_intern ({} checks)", g_passed);
    }

    std::println("\n──── test_ir_optimize_type_info_chain ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_ir_optimize_type_info_chain() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_ir_optimize_type_info_chain ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_ir_optimize_type_info_chain ({} checks)", g_passed);
    }

    std::println("\n──── test_jit_dual_string_heap ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_jit_dual_string_heap() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_jit_dual_string_heap ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_jit_dual_string_heap ({} checks)", g_passed);
    }

    std::println("\n──── test_jit_interpreter_equivalence_oracle ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_jit_interpreter_equivalence_oracle() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_jit_interpreter_equivalence_oracle ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_jit_interpreter_equivalence_oracle ({} checks)", g_passed);
    }

    std::println("\n──── test_jit_macro_deopt_hygiene ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_jit_macro_deopt_hygiene() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_jit_macro_deopt_hygiene ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_jit_macro_deopt_hygiene ({} checks)", g_passed);
    }

    std::println("\n──── test_live_closure_stable_id_only ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_live_closure_stable_id_only() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_live_closure_stable_id_only ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_live_closure_stable_id_only ({} checks)", g_passed);
    }

    std::println("\n──── test_must_deopt_before_next_call ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_must_deopt_before_next_call() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_must_deopt_before_next_call ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_must_deopt_before_next_call ({} checks)", g_passed);
    }

    std::println("\n──── test_named_closure_stable_id_at_create ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_named_closure_stable_id_at_create() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_named_closure_stable_id_at_create ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_named_closure_stable_id_at_create ({} checks)", g_passed);
    }

    std::println("\n──── test_partial_recompile_single_evict ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_partial_recompile_single_evict() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_partial_recompile_single_evict ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_partial_recompile_single_evict ({} checks)", g_passed);
    }

    std::println("\n──── test_primcall_narg ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_primcall_narg() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_primcall_narg ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_primcall_narg ({} checks)", g_passed);
    }

    std::println("\n──── test_primcall_str_intern ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_primcall_str_intern() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_primcall_str_intern ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_primcall_str_intern ({} checks)", g_passed);
    }

    std::println("\n──── test_region_priority_deopt_throttle ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_region_priority_deopt_throttle() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_region_priority_deopt_throttle ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_region_priority_deopt_throttle ({} checks)", g_passed);
    }

    std::println("\n──── test_remount_force_deopt ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_remount_force_deopt() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_remount_force_deopt ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_remount_force_deopt ({} checks)", g_passed);
    }

    std::println("\n──── test_source_to_ir_desync_recovery ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_source_to_ir_desync_recovery() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_source_to_ir_desync_recovery ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_source_to_ir_desync_recovery ({} checks)", g_passed);
    }

    std::println("\n──── test_source_to_ir_map_consistency ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_source_to_ir_map_consistency() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_source_to_ir_map_consistency ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_source_to_ir_map_consistency ({} checks)", g_passed);
    }

    std::println("\n──── test_tree_walker_fallback_strict ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_tree_walker_fallback_strict() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_tree_walker_fallback_strict ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_tree_walker_fallback_strict ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
