// test_production_hardening_batch.cpp — thematic multi-TU batch
// Stream S4 disambiguated names.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_commit_readiness_score();
extern int run_test_lock_order_production_soft();
extern int run_test_production_hardening();
extern int run_test_production_safety_p1();
extern int run_test_production_safety_p2();
extern int run_test_production_security_defaults();
extern int run_test_production_stability();
extern int run_test_stdlib_production_review();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0, members_passed = 0;
    std::println("=== test_production_hardening_batch (8 members) ===");

    std::println("\n──── test_commit_readiness_score ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_commit_readiness_score() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_commit_readiness_score");
    } else {
        ++members_passed;
        std::println("OK test_commit_readiness_score");
    }

    std::println("\n──── test_lock_order_production_soft ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_lock_order_production_soft() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_lock_order_production_soft");
    } else {
        ++members_passed;
        std::println("OK test_lock_order_production_soft");
    }

    std::println("\n──── test_production_hardening ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_hardening() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_production_hardening");
    } else {
        ++members_passed;
        std::println("OK test_production_hardening");
    }

    std::println("\n──── test_production_safety_p1 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_safety_p1() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_production_safety_p1");
    } else {
        ++members_passed;
        std::println("OK test_production_safety_p1");
    }

    std::println("\n──── test_production_safety_p2 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_safety_p2() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_production_safety_p2");
    } else {
        ++members_passed;
        std::println("OK test_production_safety_p2");
    }

    std::println("\n──── test_production_security_defaults ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_security_defaults() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_production_security_defaults");
    } else {
        ++members_passed;
        std::println("OK test_production_security_defaults");
    }

    std::println("\n──── test_production_stability ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_production_stability() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_production_stability");
    } else {
        ++members_passed;
        std::println("OK test_production_stability");
    }

    std::println("\n──── test_stdlib_production_review ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_stdlib_production_review() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL test_stdlib_production_review");
    } else {
        ++members_passed;
        std::println("OK test_stdlib_production_review");
    }

    std::println("\n=== {} ok, {} failed ===", members_passed, members_failed);
    return members_failed ? 1 : 0;
}
