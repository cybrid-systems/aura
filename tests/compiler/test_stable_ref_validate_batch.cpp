// test_stable_ref_validate_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_edsl_validate_or_refresh();
extern int run_test_stable_ref_cow_refresh_failclosed();
extern int run_test_stable_ref_export_validate();
extern int run_test_stable_ref_tenant_mandate();
extern int run_test_stable_ref_wire_v2();
extern int run_test_stable_ref_tenant_capture();
extern int run_test_stable_ref_wire_endian();
extern int run_test_validate_node_no_abort();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_stable_ref_validate_batch (8 members) ===");

    std::println("\n──── test_edsl_validate_or_refresh ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_edsl_validate_or_refresh() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_edsl_validate_or_refresh ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_edsl_validate_or_refresh ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_cow_refresh_failclosed ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_cow_refresh_failclosed() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stable_ref_cow_refresh_failclosed ({}/{})", g_passed,
                     g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_cow_refresh_failclosed ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_export_validate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_export_validate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stable_ref_export_validate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_export_validate ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_tenant_mandate ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_tenant_mandate() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stable_ref_tenant_mandate ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_tenant_mandate ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_wire_v2 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_wire_v2() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stable_ref_wire_v2 ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_wire_v2 ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_tenant_capture ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_tenant_capture() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stable_ref_tenant_capture ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_tenant_capture ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_wire_endian ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_wire_endian() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stable_ref_wire_endian ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_wire_endian ({} checks)", g_passed);
    }

    std::println("\n──── test_validate_node_no_abort ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_validate_node_no_abort() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_validate_node_no_abort ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_validate_node_no_abort ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
