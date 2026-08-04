// test_gc_misc_batch.cpp — thematic multi-TU batch
// GC heap/closures/coord leftover ACs (Stream A10b)
// Stream S2 of tests/CONSOLIDATION_PLAN.md.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_gc_closures_mtx_flush_sweep_2473();
extern int run_test_gc_coord_scope_2131();
extern int run_test_gc_heap_cells_clear_2486();
extern int run_test_atomic_mark_bitvector_2117();
extern int run_test_gc_mark_size_inject_2084();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_gc_misc_batch (5 members) ===");

    std::println("\n──── test_gc_closures_mtx_flush_sweep_2473 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_closures_mtx_flush_sweep_2473() != 0 || g_failed != 0) {
        ++members_failed;
        std::println(
            "FAIL member test_gc_closures_mtx_flush_sweep_2473 (checks: {} passed, {} failed)",
            g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_closures_mtx_flush_sweep_2473 ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_coord_scope_2131 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_coord_scope_2131() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_coord_scope_2131 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_coord_scope_2131 ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_heap_cells_clear_2486 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_heap_cells_clear_2486() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_heap_cells_clear_2486 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_heap_cells_clear_2486 ({} checks)", g_passed);
    }

    std::println("\n──── test_atomic_mark_bitvector_2117 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_atomic_mark_bitvector_2117() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_atomic_mark_bitvector_2117 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_atomic_mark_bitvector_2117 ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_mark_size_inject_2084 ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_mark_size_inject_2084() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_mark_size_inject_2084 (checks: {} passed, {} failed)",
                     g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_mark_size_inject_2084 ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
