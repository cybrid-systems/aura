// test_stable_ref_validate_batch.cpp — thematic multi-TU batch
// StableNodeRef / validate / tenant / wire ACs (Stream A6)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_edsl_validate_or_refresh_2186();
extern int run_test_stable_ref_cow_refresh_failclosed_2393();
extern int run_test_stable_ref_export_validate_2404();
extern int run_test_stable_ref_tenant_mandate_2056();
extern int run_test_stable_ref_wire_v2_2198();
extern int run_test_stable_ref_tenant_capture_2125();
extern int run_test_stable_ref_wire_endian_2395();
extern int run_test_validate_node_no_abort_2390();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_stable_ref_validate_batch (8 members) ===");

    std::println("\n──── test_edsl_validate_or_refresh_2186 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_edsl_validate_or_refresh_2186() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_edsl_validate_or_refresh_2186 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_edsl_validate_or_refresh_2186 ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_cow_refresh_failclosed_2393 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_cow_refresh_failclosed_2393() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stable_ref_cow_refresh_failclosed_2393 (checks: {} passed, "
                     "{} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_cow_refresh_failclosed_2393 ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_export_validate_2404 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_export_validate_2404() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_stable_ref_export_validate_2404 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_export_validate_2404 ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_tenant_mandate_2056 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_tenant_mandate_2056() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_stable_ref_tenant_mandate_2056 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_tenant_mandate_2056 ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_wire_v2_2198 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_wire_v2_2198() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stable_ref_wire_v2_2198 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_wire_v2_2198 ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_tenant_capture_2125 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_tenant_capture_2125() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_stable_ref_tenant_capture_2125 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_tenant_capture_2125 ({} checks)", g_passed);
    }

    std::println("\n──── test_stable_ref_wire_endian_2395 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stable_ref_wire_endian_2395() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stable_ref_wire_endian_2395 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stable_ref_wire_endian_2395 ({} checks)", g_passed);
    }

    std::println("\n──── test_validate_node_no_abort_2390 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_validate_node_no_abort_2390() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_validate_node_no_abort_2390 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_validate_node_no_abort_2390 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
