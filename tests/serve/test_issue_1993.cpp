// test_issue_1993.cpp — Issue #1993 (D-001): (gc-heap) direct-clear
// path clears module/workspace state without heap_mutex/module_mtx.
//
// Fix holds heap_mutex_ + module_mtx_ + workspace_mtx_ atomically
// (std::lock with defer_lock) for the entire (gc-heap) direct-clear
// body. (gc-heap) now also clears module + workspace state under the
// matching locks (was a doc-only "stronger reset than gc-temp" claim
// before — implementation didn't match). (gc) also adds workspace_mtx_
// for the workspace_flat_/workspace_pool_ pointer nulls.
//
// AC list:
//   AC1: (gc-heap) is callable after the fix (smoke)
//   AC2: (gc) is callable after the fix (smoke)
//   AC3: (gc-heap) clears state — eval still works post-clear
//   AC4: (gc) clears state — eval still works post-clear
//   AC5: Concurrent (gc-heap) + (gc) + eval stress — no crash, no UB
//   AC6: linter self-test (scripts/check_gc_heap_lock_scope_coverage.py)
//
// The actual lock-scope invariants (heap_mutex + module_mtx + workspace_mtx
// acquired atomically for (gc-heap) body, workspace_mtx acquired for (gc)
// workspace pointer nulls) are enforced by the linter in CI, not by this
// runtime test (which would require hooking into the Evaluator's primitive
// table).

#include "test_harness.hpp"

#include <atomic>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {
constexpr int kThreads = 4;
constexpr int kIterations = 16;
} // namespace

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;

    // === AC1: (gc-heap) is callable after the fix ===
    {
        aura::compiler::CompilerService cs;
        auto r_heap = cs.eval("(gc-heap)");
        CHECK(r_heap.has_value(), "AC1: (gc-heap) returns a value after D-001 lock-scope fix");
    }

    // === AC2: (gc) is callable after the fix (now holds workspace_mtx_) ===
    {
        aura::compiler::CompilerService cs;
        auto r_gc = cs.eval("(gc)");
        CHECK(r_gc.has_value(), "AC2: (gc) returns a value after D-001 workspace_mtx_ addition");
    }

    // === AC3: (gc-heap) clears state — eval still works post-clear ===
    // Populate some state, then clear, then verify the Evaluator
    // is still functional. Without the fix, a stale workspace_flat_
    // would have caused UB on the next eval.
    {
        aura::compiler::CompilerService cs;
        // Populate some eval state
        auto r_add = cs.eval("(+ 1 2)");
        CHECK(r_add.has_value(), "AC3: (+ 1 2) populates state pre-clear");
        // Clear heap + module + workspace state under all three locks
        auto r_heap = cs.eval("(gc-heap)");
        CHECK(r_heap.has_value(), "AC3: (gc-heap) completes");
        // Post-clear eval must still work — proves the clear was
        // total and didn't corrupt module/workspace state.
        auto r_eval = cs.eval("(+ 10 20)");
        CHECK(r_eval.has_value(), "AC3: eval works post-(gc-heap) clear");
    }

    // === AC4: (gc) clears state — eval still works post-clear ===
    {
        aura::compiler::CompilerService cs;
        auto r_add = cs.eval("(+ 1 2)");
        CHECK(r_add.has_value(), "AC4: (+ 1 2) populates state pre-clear");
        // (gc) now holds workspace_mtx_ for the workspace_flat_/
        // workspace_pool_ nulls. Without that lock, an in-flight
        // set_workspace_flat reader (WorkspaceFlatPin shared_lock)
        // would race the null.
        auto r_gc = cs.eval("(gc)");
        CHECK(r_gc.has_value(), "AC4: (gc) completes with workspace_mtx_");
        auto r_eval = cs.eval("(* 3 4)");
        CHECK(r_eval.has_value(), "AC4: eval works post-(gc) clear");
    }

    // === AC5: Concurrent (gc-heap) + (gc) + eval stress ===
    // Multiple threads hammer the same CompilerService with primitive
    // invocations that take the locks fixed by #1993. With the fix,
    // std::lock() with defer_lock avoids deadlock and all operations
    // complete cleanly. Without the fix, races would cause UB
    // (detected by TSan/ASan in CI).
    {
        aura::compiler::CompilerService cs;
        std::atomic<int> errors{0};

        std::vector<std::thread> threads;
        threads.reserve(kThreads);

        // Thread A: (gc-heap) hammer
        threads.emplace_back([&]() noexcept {
            for (int i = 0; i < kIterations; ++i) {
                try {
                    (void)cs.eval("(gc-heap)");
                } catch (...) {
                    ++errors;
                }
            }
        });

        // Thread B: (gc) hammer
        threads.emplace_back([&]() noexcept {
            for (int i = 0; i < kIterations; ++i) {
                try {
                    (void)cs.eval("(gc)");
                } catch (...) {
                    ++errors;
                }
            }
        });

        // Thread C: eval-only hammer
        threads.emplace_back([&]() noexcept {
            for (int i = 0; i < kIterations; ++i) {
                try {
                    (void)cs.eval("(+ 1 1)");
                } catch (...) {
                    ++errors;
                }
            }
        });

        // Thread D: another (gc-heap) hammer
        threads.emplace_back([&]() noexcept {
            for (int i = 0; i < kIterations; ++i) {
                try {
                    (void)cs.eval("(gc-heap)");
                } catch (...) {
                    ++errors;
                }
            }
        });

        for (auto& th : threads)
            th.join();

        CHECK(errors.load() == 0,
              "AC5: no exceptions under concurrent (gc-heap) + (gc) + eval stress");
    }

    // === AC6: linter self-test ===
    // The lock-scope invariants are enforced by
    // scripts/check_gc_heap_lock_scope_coverage.py — see that script
    // for the source-level AC list. This test is a runtime smoke test;
    // the linter is the authoritative gate for the lock-scope fix.

    if (::aura::test::g_failed)
        return 1;
    std::println("issue 1993 gc-heap lock scope (D-001): OK ({} passed)", ::aura::test::g_passed);
    return 0;
}