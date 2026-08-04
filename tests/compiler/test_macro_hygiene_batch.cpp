// test_macro_hygiene_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_hygiene_checkpoint();
extern int run_test_macro_cross_flat_hygiene();
extern int run_test_macro_hygiene_limits();
extern int run_test_rest_param_hygiene();
extern int run_test_rest_param_nested_qq_hygiene();
extern int run_test_hygiene_diagnostic();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_macro_hygiene_batch (6 members) ===");

    std::println("\n──── test_hygiene_checkpoint ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hygiene_checkpoint() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hygiene_checkpoint ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hygiene_checkpoint ({} checks)", g_passed);
    }

    std::println("\n──── test_macro_cross_flat_hygiene ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_macro_cross_flat_hygiene() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_macro_cross_flat_hygiene ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_macro_cross_flat_hygiene ({} checks)", g_passed);
    }

    std::println("\n──── test_macro_hygiene_limits ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_macro_hygiene_limits() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_macro_hygiene_limits ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_macro_hygiene_limits ({} checks)", g_passed);
    }

    std::println("\n──── test_rest_param_hygiene ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_rest_param_hygiene() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_rest_param_hygiene ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_rest_param_hygiene ({} checks)", g_passed);
    }

    std::println("\n──── test_rest_param_nested_qq_hygiene ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_rest_param_nested_qq_hygiene() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_rest_param_nested_qq_hygiene ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_rest_param_nested_qq_hygiene ({} checks)", g_passed);
    }

    std::println("\n──── test_hygiene_diagnostic ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hygiene_diagnostic() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hygiene_diagnostic ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_hygiene_diagnostic ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
