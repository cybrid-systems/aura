// test_arena_compact_hooks_batch.cpp — thematic multi-TU batch
// Arena compact hooks / dtor / force compact ACs (Stream A9)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_arena_adaptive_compact_2059();
extern int run_test_arena_compact_hook_stats_2381();
extern int run_test_arena_compact_notify_lifecycle_2438();
extern int run_test_arena_dtor_clears_hooks_2382();
extern int run_test_force_compact_hard_mutex_2157();
extern int run_test_has_on_compact_hook_lock_2383();
extern int run_test_incremental_restamp_2061();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_arena_compact_hooks_batch (7 members) ===");

    std::println("\n──── test_arena_adaptive_compact_2059 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_arena_adaptive_compact_2059() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_arena_adaptive_compact_2059 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_arena_adaptive_compact_2059 ({} checks)", g_passed);
    }

    std::println("\n──── test_arena_compact_hook_stats_2381 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_arena_compact_hook_stats_2381() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_arena_compact_hook_stats_2381 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_arena_compact_hook_stats_2381 ({} checks)", g_passed);
    }

    std::println("\n──── test_arena_compact_notify_lifecycle_2438 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_arena_compact_notify_lifecycle_2438() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_arena_compact_notify_lifecycle_2438 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_arena_compact_notify_lifecycle_2438 ({} checks)", g_passed);
    }

    std::println("\n──── test_arena_dtor_clears_hooks_2382 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_arena_dtor_clears_hooks_2382() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_arena_dtor_clears_hooks_2382 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_arena_dtor_clears_hooks_2382 ({} checks)", g_passed);
    }

    std::println("\n──── test_force_compact_hard_mutex_2157 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_force_compact_hard_mutex_2157() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_force_compact_hard_mutex_2157 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_force_compact_hard_mutex_2157 ({} checks)", g_passed);
    }

    std::println("\n──── test_has_on_compact_hook_lock_2383 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_has_on_compact_hook_lock_2383() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_has_on_compact_hook_lock_2383 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_has_on_compact_hook_lock_2383 ({} checks)", g_passed);
    }

    std::println("\n──── test_incremental_restamp_2061 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_incremental_restamp_2061() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_incremental_restamp_2061 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_incremental_restamp_2061 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
