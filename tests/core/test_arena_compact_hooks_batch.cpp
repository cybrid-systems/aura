// test_arena_compact_hooks_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_arena_adaptive_compact();
extern int run_test_arena_compact_hook_stats();
extern int run_test_arena_compact_notify_lifecycle();
extern int run_test_arena_dtor_clears_hooks();
extern int run_test_force_compact_hard_mutex();
extern int run_test_has_on_compact_hook_lock();
extern int run_test_incremental_restamp();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_arena_compact_hooks_batch (7 members) ===");

    std::println("\n──── test_arena_adaptive_compact ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_arena_adaptive_compact() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_arena_adaptive_compact ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_arena_adaptive_compact ({} checks)", g_passed);
    }

    std::println("\n──── test_arena_compact_hook_stats ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_arena_compact_hook_stats() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_arena_compact_hook_stats ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_arena_compact_hook_stats ({} checks)", g_passed);
    }

    std::println("\n──── test_arena_compact_notify_lifecycle ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_arena_compact_notify_lifecycle() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_arena_compact_notify_lifecycle ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_arena_compact_notify_lifecycle ({} checks)", g_passed);
    }

    std::println("\n──── test_arena_dtor_clears_hooks ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_arena_dtor_clears_hooks() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_arena_dtor_clears_hooks ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_arena_dtor_clears_hooks ({} checks)", g_passed);
    }

    std::println("\n──── test_force_compact_hard_mutex ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_force_compact_hard_mutex() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_force_compact_hard_mutex ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_force_compact_hard_mutex ({} checks)", g_passed);
    }

    std::println("\n──── test_has_on_compact_hook_lock ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_has_on_compact_hook_lock() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_has_on_compact_hook_lock ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_has_on_compact_hook_lock ({} checks)", g_passed);
    }

    std::println("\n──── test_incremental_restamp ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_incremental_restamp() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_incremental_restamp ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_incremental_restamp ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
