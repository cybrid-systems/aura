// test_cross_cow_batch.cpp — thematic multi-TU batch
// Cross-COW soft migrate / drift contract (Stream A10f)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_cross_cow_drift_contract_2505();
extern int run_test_cross_cow_soft_migrate_2371();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_cross_cow_batch (2 members) ===");

    std::println("\n──── test_cross_cow_drift_contract_2505 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_cross_cow_drift_contract_2505() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_cross_cow_drift_contract_2505 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cross_cow_drift_contract_2505 ({} checks)", g_passed);
    }

    std::println("\n──── test_cross_cow_soft_migrate_2371 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_cross_cow_soft_migrate_2371() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_cross_cow_soft_migrate_2371 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_cross_cow_soft_migrate_2371 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
