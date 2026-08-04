// test_pcv_workspace_batch.cpp — thematic multi-TU batch
// PCV / workspace isolation & contention ACs (Stream A8)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_workspace_mtx_contention_2523();
extern int run_test_workspace_region_concurrency_2121();
extern int run_test_pcv_exclusive_with_set_2140();
extern int run_test_pcv_tls_default_on_2521();
extern int run_test_pcv_tls_scratch_2406();
extern int run_test_pcv_unique_hotpath_2058();
extern int run_test_workspace_isolation_wire_2073();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_pcv_workspace_batch (7 members) ===");

    std::println("\n──── test_workspace_mtx_contention_2523 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_workspace_mtx_contention_2523() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_workspace_mtx_contention_2523 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_workspace_mtx_contention_2523 ({} checks)", g_passed);
    }

    std::println("\n──── test_workspace_region_concurrency_2121 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_workspace_region_concurrency_2121() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_workspace_region_concurrency_2121 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_workspace_region_concurrency_2121 ({} checks)", g_passed);
    }

    std::println("\n──── test_pcv_exclusive_with_set_2140 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_pcv_exclusive_with_set_2140() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_pcv_exclusive_with_set_2140 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_pcv_exclusive_with_set_2140 ({} checks)", g_passed);
    }

    std::println("\n──── test_pcv_tls_default_on_2521 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_pcv_tls_default_on_2521() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_pcv_tls_default_on_2521 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_pcv_tls_default_on_2521 ({} checks)", g_passed);
    }

    std::println("\n──── test_pcv_tls_scratch_2406 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_pcv_tls_scratch_2406() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_pcv_tls_scratch_2406 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_pcv_tls_scratch_2406 ({} checks)", g_passed);
    }

    std::println("\n──── test_pcv_unique_hotpath_2058 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_pcv_unique_hotpath_2058() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_pcv_unique_hotpath_2058 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_pcv_unique_hotpath_2058 ({} checks)", g_passed);
    }

    std::println("\n──── test_workspace_isolation_wire_2073 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_workspace_isolation_wire_2073() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_workspace_isolation_wire_2073 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_workspace_isolation_wire_2073 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
