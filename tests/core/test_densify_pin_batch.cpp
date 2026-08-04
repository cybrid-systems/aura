// test_densify_pin_batch.cpp — thematic multi-TU batch
// Densify / pin / Moving / envframe ownership
// Members export run_<name>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.
// Policy: tests/HOMES.md — extend this batch, do not add test_*_<issue>.cpp.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_arena_moving_densify_health_2619();
extern int run_test_densify_envframe_ok_2361();
extern int run_test_densify_last_call_axes_2376();
extern int run_test_densify_ownership_scan_fail_gate_2497();
extern int run_test_densify_remap_pairing_2368();
extern int run_test_densify_root_closure_closed_loop_2365();
extern int run_test_envframe_ownership_steal_densify_2362();
extern int run_test_escape_gate_steal_densify_clear_2507();
extern int run_test_lifetime_contract_snapshot_2300();
extern int run_test_linear_pin_moving_compact_2280();
extern int run_test_panic_defer_after_densify_2364();
extern int run_test_post_densify_linear_type_revalidate_2353();
extern int run_test_root_remap_pass_2267();
extern int run_test_root_remap_pin_contract_unified_2499();
extern int run_test_stable_ref_pin_lifecycle_2189();
extern int run_test_type_freshness_steal_densify_2552();
extern int run_test_general_object_pin_2298();
extern int run_test_general_object_pin_adopt_2363();
extern int run_test_general_object_pin_coverage_gate_2496();
extern int run_test_moving_compact_2166();
extern int run_test_moving_densify_fail_closed_2495();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_densify_pin_batch (21 members) ===");

    std::println("\n──── test_arena_moving_densify_health_2619 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_arena_moving_densify_health_2619() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_arena_moving_densify_health_2619 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_arena_moving_densify_health_2619 ({} checks)", g_passed);
    }

    std::println("\n──── test_densify_envframe_ok_2361 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_densify_envframe_ok_2361() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_densify_envframe_ok_2361 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_densify_envframe_ok_2361 ({} checks)", g_passed);
    }

    std::println("\n──── test_densify_last_call_axes_2376 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_densify_last_call_axes_2376() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_densify_last_call_axes_2376 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_densify_last_call_axes_2376 ({} checks)", g_passed);
    }

    std::println("\n──── test_densify_ownership_scan_fail_gate_2497 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_densify_ownership_scan_fail_gate_2497() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_densify_ownership_scan_fail_gate_2497 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_densify_ownership_scan_fail_gate_2497 ({} checks)", g_passed);
    }

    std::println("\n──── test_densify_remap_pairing_2368 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_densify_remap_pairing_2368() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_densify_remap_pairing_2368 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_densify_remap_pairing_2368 ({} checks)", g_passed);
    }

    std::println("\n──── test_densify_root_closure_closed_loop_2365 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_densify_root_closure_closed_loop_2365() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_densify_root_closure_closed_loop_2365 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_densify_root_closure_closed_loop_2365 ({} checks)", g_passed);
    }

    std::println("\n──── test_envframe_ownership_steal_densify_2362 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_envframe_ownership_steal_densify_2362() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_envframe_ownership_steal_densify_2362 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_envframe_ownership_steal_densify_2362 ({} checks)", g_passed);
    }

    std::println("\n──── test_escape_gate_steal_densify_clear_2507 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_escape_gate_steal_densify_clear_2507() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_escape_gate_steal_densify_clear_2507 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_escape_gate_steal_densify_clear_2507 ({} checks)", g_passed);
    }

    std::println("\n──── test_lifetime_contract_snapshot_2300 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_lifetime_contract_snapshot_2300() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_lifetime_contract_snapshot_2300 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_lifetime_contract_snapshot_2300 ({} checks)", g_passed);
    }

    std::println("\n──── test_linear_pin_moving_compact_2280 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_linear_pin_moving_compact_2280() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_linear_pin_moving_compact_2280 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_linear_pin_moving_compact_2280 ({} checks)", g_passed);
    }

    std::println("\n──── test_panic_defer_after_densify_2364 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_panic_defer_after_densify_2364() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_panic_defer_after_densify_2364 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_panic_defer_after_densify_2364 ({} checks)", g_passed);
    }

    std::println("\n──── test_post_densify_linear_type_revalidate_2353 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_post_densify_linear_type_revalidate_2353() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_post_densify_linear_type_revalidate_2353 (checks: {} "
                     "passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_post_densify_linear_type_revalidate_2353 ({} checks)",
                     g_passed);
    }

    std::println("\n──── test_root_remap_pass_2267 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_root_remap_pass_2267() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_root_remap_pass_2267 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_root_remap_pass_2267 ({} checks)", g_passed);
    }

    std::println("\n──── test_root_remap_pin_contract_unified_2499 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_root_remap_pin_contract_unified_2499() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_root_remap_pin_contract_unified_2499 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_root_remap_pin_contract_unified_2499 ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_pin_lifecycle_2189 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_pin_lifecycle_2189() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_stable_ref_pin_lifecycle_2189 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_pin_lifecycle_2189 ({} checks)", g_passed);
    }

    std::println("\n──── test_type_freshness_steal_densify_2552 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_type_freshness_steal_densify_2552() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_type_freshness_steal_densify_2552 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_type_freshness_steal_densify_2552 ({} checks)", g_passed);
    }

    std::println("\n──── test_general_object_pin_2298 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_general_object_pin_2298() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_general_object_pin_2298 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_general_object_pin_2298 ({} checks)", g_passed);
    }

    std::println("\n──── test_general_object_pin_adopt_2363 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_general_object_pin_adopt_2363() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_general_object_pin_adopt_2363 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_general_object_pin_adopt_2363 ({} checks)", g_passed);
    }

    std::println("\n──── test_general_object_pin_coverage_gate_2496 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_general_object_pin_coverage_gate_2496() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_general_object_pin_coverage_gate_2496 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_general_object_pin_coverage_gate_2496 ({} checks)", g_passed);
    }

    std::println("\n──── test_moving_compact_2166 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_moving_compact_2166() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_moving_compact_2166 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_moving_compact_2166 ({} checks)", g_passed);
    }

    std::println("\n──── test_moving_densify_fail_closed_2495 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_moving_densify_fail_closed_2495() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_moving_densify_fail_closed_2495 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_moving_densify_fail_closed_2495 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
