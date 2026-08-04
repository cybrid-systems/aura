// test_reemit_defer_batch.cpp — thematic multi-TU batch
// Reemit production default defer (Stream C soft-home for 2205/2208)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_reemit_production_default_defer_2205();
extern int run_test_reemit_production_default_defer_2208();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_reemit_defer_batch (2 members) ===");

    std::println("\n──── test_reemit_production_default_defer_2205 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reemit_production_default_defer_2205() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_reemit_production_default_defer_2205 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_reemit_production_default_defer_2205 ({} checks)", g_passed);
    }

    std::println("\n──── test_reemit_production_default_defer_2208 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_reemit_production_default_defer_2208() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_reemit_production_default_defer_2208 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_reemit_production_default_defer_2208 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
