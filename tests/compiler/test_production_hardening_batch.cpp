// test_production_hardening_batch.cpp — thematic multi-TU batch
// Production hardening / safety / readiness ACs (Stream A7)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_commit_readiness_score_2553();
extern int run_test_lock_order_production_soft_2557();
extern int run_test_production_hardening_985();
extern int run_test_production_safety_1047();
extern int run_test_production_safety_1097();
extern int run_test_production_security_defaults_2053();
extern int run_test_production_stability_1014();
extern int run_test_stdlib_production_review_923();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_production_hardening_batch (8 members) ===");

    std::println("\n──── test_commit_readiness_score_2553 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_commit_readiness_score_2553() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_commit_readiness_score_2553 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_commit_readiness_score_2553 ({} checks)", g_passed);
    }

    std::println("\n──── test_lock_order_production_soft_2557 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_lock_order_production_soft_2557() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_lock_order_production_soft_2557 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_lock_order_production_soft_2557 ({} checks)", g_passed);
    }

    std::println("\n──── test_production_hardening_985 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_hardening_985() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_production_hardening_985 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_production_hardening_985 ({} checks)", g_passed);
    }

    std::println("\n──── test_production_safety_1047 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_safety_1047() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_production_safety_1047 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_production_safety_1047 ({} checks)", g_passed);
    }

    std::println("\n──── test_production_safety_1097 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_safety_1097() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_production_safety_1097 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_production_safety_1097 ({} checks)", g_passed);
    }

    std::println("\n──── test_production_security_defaults_2053 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_security_defaults_2053() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_production_security_defaults_2053 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_production_security_defaults_2053 ({} checks)", g_passed);
    }

    std::println("\n──── test_production_stability_1014 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_stability_1014() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_production_stability_1014 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_production_stability_1014 ({} checks)", g_passed);
    }

    std::println("\n──── test_stdlib_production_review_923 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stdlib_production_review_923() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_stdlib_production_review_923 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_stdlib_production_review_923 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
