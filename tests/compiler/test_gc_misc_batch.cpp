// test_gc_misc_batch.cpp — thematic multi-TU batch
// Stream S4: member filenames stripped of _NNNN issue suffixes where unique.
// Members: run_<stem>(); standalones keep main via #ifndef AURA_ISSUE_BATCH_MEMBER.

#include "test_harness.hpp"

#include <print>

import std;

extern int run_test_gc_closures_mtx_flush_sweep();
extern int run_test_gc_coord_scope();
extern int run_test_gc_heap_cells_clear();
extern int run_test_atomic_mark_bitvector();
extern int run_test_gc_mark_size_inject();

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    int members_failed = 0;
    int members_passed = 0;
    std::println("=== test_gc_misc_batch (5 members) ===");

    std::println("\n──── test_gc_closures_mtx_flush_sweep ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_closures_mtx_flush_sweep() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_closures_mtx_flush_sweep ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_closures_mtx_flush_sweep ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_coord_scope ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_coord_scope() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_coord_scope ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_coord_scope ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_heap_cells_clear ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_heap_cells_clear() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_heap_cells_clear ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_heap_cells_clear ({} checks)", g_passed);
    }

    std::println("\n──── test_atomic_mark_bitvector ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_atomic_mark_bitvector() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_atomic_mark_bitvector ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_atomic_mark_bitvector ({} checks)", g_passed);
    }

    std::println("\n──── test_gc_mark_size_inject ────");
    g_passed = 0;
    g_failed = 0;
    if (run_test_gc_mark_size_inject() != 0 || g_failed != 0) {
        ++members_failed;
        std::println("FAIL member test_gc_mark_size_inject ({}/{})", g_passed, g_failed);
    } else {
        ++members_passed;
        std::println("OK member test_gc_mark_size_inject ({} checks)", g_passed);
    }

    std::println("\n=== {} members: {} ok, {} failed ===", members_passed + members_failed,
                 members_passed, members_failed);
    return members_failed ? 1 : 0;
}
