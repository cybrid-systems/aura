// test_module_query_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_module_export_display();
extern int run_test_module_load_tail_export();
extern int run_test_module_partition_map();
extern int run_test_module_rebind_residual();
extern int run_test_module_require_freevar();
extern int run_test_query_and_replace_batch();
extern int run_test_query_by_marker_provenance();
extern int run_test_query_epoch_contract();
extern int run_test_query_hygiene_default();
extern int run_test_query_index_composite();
extern int run_test_query_pattern_default_hygiene();
extern int run_test_setcode_rebind_survive();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_module_query_batch (12 members) ===");

    std::println("\n──── test_module_export_display ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_module_export_display() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_module_export_display ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_module_export_display ({} checks)", g_passed);
    }

    std::println("\n──── test_module_load_tail_export ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_module_load_tail_export() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_module_load_tail_export ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_module_load_tail_export ({} checks)", g_passed);
    }

    std::println("\n──── test_module_partition_map ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_module_partition_map() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_module_partition_map ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_module_partition_map ({} checks)", g_passed);
    }

    std::println("\n──── test_module_rebind_residual ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_module_rebind_residual() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_module_rebind_residual ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_module_rebind_residual ({} checks)", g_passed);
    }

    std::println("\n──── test_module_require_freevar ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_module_require_freevar() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_module_require_freevar ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_module_require_freevar ({} checks)", g_passed);
    }

    std::println("\n──── test_query_and_replace_batch ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_query_and_replace_batch() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_query_and_replace_batch ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_query_and_replace_batch ({} checks)", g_passed);
    }

    std::println("\n──── test_query_by_marker_provenance ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_query_by_marker_provenance() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_query_by_marker_provenance ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_query_by_marker_provenance ({} checks)", g_passed);
    }

    std::println("\n──── test_query_epoch_contract ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_query_epoch_contract() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_query_epoch_contract ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_query_epoch_contract ({} checks)", g_passed);
    }

    std::println("\n──── test_query_hygiene_default ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_query_hygiene_default() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_query_hygiene_default ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_query_hygiene_default ({} checks)", g_passed);
    }

    std::println("\n──── test_query_index_composite ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_query_index_composite() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_query_index_composite ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_query_index_composite ({} checks)", g_passed);
    }

    std::println("\n──── test_query_pattern_default_hygiene ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_query_pattern_default_hygiene() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_query_pattern_default_hygiene ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_query_pattern_default_hygiene ({} checks)", g_passed);
    }

    std::println("\n──── test_setcode_rebind_survive ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_setcode_rebind_survive() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_setcode_rebind_survive ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_setcode_rebind_survive ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
