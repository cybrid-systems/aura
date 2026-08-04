// test_macro_hygiene_batch.cpp — thematic multi-TU batch
// Macro hygiene / rest-param / checkpoint ACs (Stream A10d+)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_hygiene_checkpoint_2099();
extern int run_test_macro_cross_flat_hygiene_2235();
extern int run_test_macro_hygiene_limits_2101();
extern int run_test_hygiene_diagnostic_2167();
extern int run_test_rest_param_hygiene_2169();
extern int run_test_rest_param_nested_qq_hygiene_2239();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0, members_passed = 0;
    std::println("=== test_macro_hygiene_batch (6 members) ===");

    std::println("\n──── test_hygiene_checkpoint_2099 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hygiene_checkpoint_2099() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hygiene_checkpoint_2099");
    } else {
        ++members_passed;
        std::println("OK member test_hygiene_checkpoint_2099 ({} checks)", g_passed);
    }

    std::println("\n──── test_macro_cross_flat_hygiene_2235 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_macro_cross_flat_hygiene_2235() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_macro_cross_flat_hygiene_2235");
    } else {
        ++members_passed;
        std::println("OK member test_macro_cross_flat_hygiene_2235 ({} checks)", g_passed);
    }

    std::println("\n──── test_macro_hygiene_limits_2101 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_macro_hygiene_limits_2101() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_macro_hygiene_limits_2101");
    } else {
        ++members_passed;
        std::println("OK member test_macro_hygiene_limits_2101 ({} checks)", g_passed);
    }

    std::println("\n──── test_hygiene_diagnostic_2167 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_hygiene_diagnostic_2167() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_hygiene_diagnostic_2167");
    } else {
        ++members_passed;
        std::println("OK member test_hygiene_diagnostic_2167 ({} checks)", g_passed);
    }

    std::println("\n──── test_rest_param_hygiene_2169 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_rest_param_hygiene_2169() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_rest_param_hygiene_2169");
    } else {
        ++members_passed;
        std::println("OK member test_rest_param_hygiene_2169 ({} checks)", g_passed);
    }

    std::println("\n──── test_rest_param_nested_qq_hygiene_2239 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_rest_param_nested_qq_hygiene_2239() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_rest_param_nested_qq_hygiene_2239");
    } else {
        ++members_passed;
        std::println("OK member test_rest_param_nested_qq_hygiene_2239 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
