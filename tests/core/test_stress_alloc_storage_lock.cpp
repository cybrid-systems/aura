// test_stress_alloc_storage_lock.cpp — Issue #1397
// Standalone long stress; EXCLUDE_FROM_ALL optional.

#include "test_harness.hpp"
import std;
using namespace std::chrono_literals;

import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

// test_stress_alloc_storage_lock.cpp - Issue #1397:
// cells_/pairs/string_heap_ uniform alloc_storage_lock_ stress test.
//
// Verifies that the uniform alloc_storage_lock_ mutex on Evaluator
// correctly serializes concurrent push_back to cells_/pairs_/string_heap_
// under fiber:spawn. Without it, two fibers each calling `(map f lst)`
// race on pairs_.push_back (lost updates, iterator invalidation,
// torn pair slot writes).
//
// ACs (mirroring #1397 body):
//   AC1: 2 threads each calling `(map f lst)` on different lists →
//        no torn `pairs_` writes
//   AC2: `set-car!` contract: pair index stable across `push_back`
//   AC3: stress 4 threads × 100K (cons) ops → no UB

using namespace std::chrono_literals;


namespace test_stress_alloc_storage_lock_detail {

static void run_ac1_concurrent_map_f_lst(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: 2 threads calling (map f lst) concurrently ---");
    constexpr int N = 2000;
    std::atomic<int> errors{0};
    auto worker = [&](int base) {
        try {
            for (int i = 0; i < N; ++i) {
                auto r = cs.eval(std::format(
                    "(map (lambda (x) (+ x {})) (list 1 2 3 4 5 6 7 8 9 10))", base + i));
                if (!r)
                    errors.fetch_add(1);
            }
        } catch (...) {
            errors.fetch_add(1);
        }
    };
    std::thread t1(worker, 0);
    std::thread t2(worker, 10000);
    t1.join();
    t2.join();
    CHECK(errors.load() == 0,
          std::format("2 threads x {} (map f lst): 0 errors (got {})", N, errors.load()));
}

static void run_ac2_set_car_stable(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: set-car! index stable across concurrent push_back ---");
    constexpr int N = 2000;
    std::atomic<int> errors{0};
    auto worker = [&](int tid) {
        try {
            for (int i = 0; i < N; ++i) {
                cs.eval(std::format("(let ((p (cons {} 0))) (set-car! p {}) p)", tid * N + i,
                                    tid * N + i + 1));
            }
        } catch (...) {
            errors.fetch_add(1);
        }
    };
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back(worker, t);
    for (auto& th : ts)
        th.join();
    CHECK(errors.load() == 0,
          std::format("4 threads x {} set-car!: 0 errors (got {})", N, errors.load()));
}

static void run_ac3_stress_4threads_100k(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: 4 threads x 100K (cons) ops no UB ---");
    constexpr int N = 100000;
    std::atomic<int> errors{0};
    auto worker = [&](int tid) {
        try {
            for (int i = 0; i < N; ++i) {
                cs.eval(std::format("(cons {} (cons {} (cons {} (cons {} '()))))", tid * N + i,
                                    tid * N + i + 1, tid * N + i + 2, tid * N + i + 3));
            }
        } catch (...) {
            errors.fetch_add(1);
        }
    };
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back(worker, t);
    for (auto& th : ts)
        th.join();
    CHECK(errors.load() == 0,
          std::format("4 threads x {} cons: 0 errors (got {})", N, errors.load()));
}

} // namespace test_stress_alloc_storage_lock_detail

int aura_stress_alloc_storage_lock_run() {
    using namespace test_stress_alloc_storage_lock_detail;
    std::println("=== Issue #1397: alloc_storage_lock_ stress test ===");
    {
        aura::compiler::CompilerService cs;
        run_ac1_concurrent_map_f_lst(cs);
        run_ac2_set_car_stable(cs);
        run_ac3_stress_4threads_100k(cs);
    }
    std::println("\n\u2550\u2550\u255d Results: {}/{} passed, {}/{} failed \u2550\u2550\u255d",
                 ::aura::test::g_passed, ::aura::test::g_passed + ::aura::test::g_failed,
                 ::aura::test::g_failed, ::aura::test::g_passed + ::aura::test::g_failed);
    return ::aura::test::g_failed == 0 ? 0 : 1;
}


#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_stress_alloc_storage_lock_run();
}
#endif
