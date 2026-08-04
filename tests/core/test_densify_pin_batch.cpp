// test_densify_pin_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_arena_moving_densify_health();
extern int run_test_densify_envframe_ok();
extern int run_test_densify_last_call_axes();
extern int run_test_densify_ownership_scan_fail_gate();
extern int run_test_densify_remap_pairing();
extern int run_test_densify_root_closure_closed_loop();
extern int run_test_envframe_ownership_steal_densify();
extern int run_test_escape_gate_steal_densify_clear();
extern int run_test_lifetime_contract_snapshot();
extern int run_test_linear_pin_moving_compact();
extern int run_test_panic_defer_after_densify();
extern int run_test_post_densify_linear_type_revalidate();
extern int run_test_root_remap_pass();
extern int run_test_root_remap_pin_contract_unified();
extern int run_test_stable_ref_pin_lifecycle();
extern int run_test_type_freshness_steal_densify();
extern int run_test_general_object_pin();
extern int run_test_general_object_pin_adopt();
extern int run_test_general_object_pin_coverage_gate();
extern int run_test_moving_compact();
extern int run_test_moving_densify_fail_closed();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_densify_pin_batch (21 members) ===");

    std::println("\n──── test_arena_moving_densify_health ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_arena_moving_densify_health() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_arena_moving_densify_health ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_arena_moving_densify_health ({} checks)", g_passed);
    }

    std::println("\n──── test_densify_envframe_ok ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_densify_envframe_ok() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_densify_envframe_ok ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_densify_envframe_ok ({} checks)", g_passed);
    }

    std::println("\n──── test_densify_last_call_axes ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_densify_last_call_axes() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_densify_last_call_axes ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_densify_last_call_axes ({} checks)", g_passed);
    }

    std::println("\n──── test_densify_ownership_scan_fail_gate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_densify_ownership_scan_fail_gate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_densify_ownership_scan_fail_gate ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_densify_ownership_scan_fail_gate ({} checks)", g_passed);
    }

    std::println("\n──── test_densify_remap_pairing ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_densify_remap_pairing() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_densify_remap_pairing ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_densify_remap_pairing ({} checks)", g_passed);
    }

    std::println("\n──── test_densify_root_closure_closed_loop ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_densify_root_closure_closed_loop() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_densify_root_closure_closed_loop ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_densify_root_closure_closed_loop ({} checks)", g_passed);
    }

    std::println("\n──── test_envframe_ownership_steal_densify ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_envframe_ownership_steal_densify() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_envframe_ownership_steal_densify ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_envframe_ownership_steal_densify ({} checks)", g_passed);
    }

    std::println("\n──── test_escape_gate_steal_densify_clear ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_escape_gate_steal_densify_clear() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_escape_gate_steal_densify_clear ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_escape_gate_steal_densify_clear ({} checks)", g_passed);
    }

    std::println("\n──── test_lifetime_contract_snapshot ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_lifetime_contract_snapshot() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_lifetime_contract_snapshot ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_lifetime_contract_snapshot ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_pin_moving_compact ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_pin_moving_compact() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_linear_pin_moving_compact ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_pin_moving_compact ({} checks)", g_passed);
    }

    std::println("\n──── test_panic_defer_after_densify ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_panic_defer_after_densify() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_panic_defer_after_densify ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_panic_defer_after_densify ({} checks)", g_passed);
    }

    std::println("\n──── test_post_densify_linear_type_revalidate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_post_densify_linear_type_revalidate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_post_densify_linear_type_revalidate ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_post_densify_linear_type_revalidate ({} checks)", g_passed);
    }

    std::println("\n──── test_root_remap_pass ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_root_remap_pass() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_root_remap_pass ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_root_remap_pass ({} checks)", g_passed);
    }

    std::println("\n──── test_root_remap_pin_contract_unified ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_root_remap_pin_contract_unified() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_root_remap_pin_contract_unified ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_root_remap_pin_contract_unified ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_pin_lifecycle ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_pin_lifecycle() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stable_ref_pin_lifecycle ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_pin_lifecycle ({} checks)", g_passed);
    }

    std::println("\n──── test_type_freshness_steal_densify ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_freshness_steal_densify() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_type_freshness_steal_densify ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_freshness_steal_densify ({} checks)", g_passed);
    }

    std::println("\n──── test_general_object_pin ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_general_object_pin() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_general_object_pin ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_general_object_pin ({} checks)", g_passed);
    }

    std::println("\n──── test_general_object_pin_adopt ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_general_object_pin_adopt() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_general_object_pin_adopt ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_general_object_pin_adopt ({} checks)", g_passed);
    }

    std::println("\n──── test_general_object_pin_coverage_gate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_general_object_pin_coverage_gate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_general_object_pin_coverage_gate ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_general_object_pin_coverage_gate ({} checks)", g_passed);
    }

    std::println("\n──── test_moving_compact ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_moving_compact() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_moving_compact ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_moving_compact ({} checks)", g_passed);
    }

    std::println("\n──── test_moving_densify_fail_closed ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_moving_densify_fail_closed() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_moving_densify_fail_closed ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_moving_densify_fail_closed ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
