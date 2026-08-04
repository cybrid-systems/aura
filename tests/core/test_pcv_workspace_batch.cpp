// test_pcv_workspace_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_workspace_mtx_contention();
extern int run_test_workspace_region_concurrency();
extern int run_test_pcv_exclusive_with_set();
extern int run_test_pcv_tls_default_on();
extern int run_test_pcv_tls_scratch();
extern int run_test_pcv_unique_hotpath();
extern int run_test_workspace_isolation_wire();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_pcv_workspace_batch (7 members) ===");

    std::println("\n──── test_workspace_mtx_contention ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_workspace_mtx_contention() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_workspace_mtx_contention ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_workspace_mtx_contention ({} checks)", g_passed);
    }

    std::println("\n──── test_workspace_region_concurrency ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_workspace_region_concurrency() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_workspace_region_concurrency ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_workspace_region_concurrency ({} checks)", g_passed);
    }

    std::println("\n──── test_pcv_exclusive_with_set ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_pcv_exclusive_with_set() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_pcv_exclusive_with_set ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_pcv_exclusive_with_set ({} checks)", g_passed);
    }

    std::println("\n──── test_pcv_tls_default_on ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_pcv_tls_default_on() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_pcv_tls_default_on ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_pcv_tls_default_on ({} checks)", g_passed);
    }

    std::println("\n──── test_pcv_tls_scratch ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_pcv_tls_scratch() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_pcv_tls_scratch ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_pcv_tls_scratch ({} checks)", g_passed);
    }

    std::println("\n──── test_pcv_unique_hotpath ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_pcv_unique_hotpath() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_pcv_unique_hotpath ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_pcv_unique_hotpath ({} checks)", g_passed);
    }

    std::println("\n──── test_workspace_isolation_wire ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_workspace_isolation_wire() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_workspace_isolation_wire ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_workspace_isolation_wire ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
