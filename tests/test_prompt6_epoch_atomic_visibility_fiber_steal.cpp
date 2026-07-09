// test_prompt6_epoch_atomic_visibility_fiber_steal.cpp — Issue #739:
// Atomic epoch visibility + memory_order under invalidate_function
// bump vs live apply_closure / GuardShape hot paths under fiber steal.
//
// Non-duplicative with #719 (epoch check exists), #718, #720, #657.
//
//   - AC1: query:closure-epoch-concurrency-stats reachable (schema 739)
//   - AC2: fence-enforced bumps on closure apply
//   - AC3: invalidate_function release bump + safe fallback path
//   - AC4: multi-fiber steal/yield + concurrent invalidate stress
//   - AC5: metrics monotonic over stress matrix
//   - AC6: query regression (closure-stats, compile:epoch)

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "serve/scheduler.h"
#include "serve/worker.h"

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

extern "C" void aura_evaluator_test_push_mutation_checkpoint();
extern "C" void aura_evaluator_test_pop_mutation_checkpoint();

namespace aura_issue_739_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::Scheduler;
using aura::serve::YieldReason;

static std::int64_t epoch_hash(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (query:closure-epoch-concurrency-stats) \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto stale = epoch_hash(cs, "stale-epoch-on-steal");
    const auto fence = epoch_hash(cs, "fence-enforced");
    const auto linear = epoch_hash(cs, "linear-violation-prevented");
    if (stale < 0 || fence < 0 || linear < 0)
        return -1;
    return stale + fence + linear;
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:closure-epoch-concurrency-stats (schema 739) ---");
    auto h = cs.eval("(query:closure-epoch-concurrency-stats)");
    CHECK(h && is_hash(*h), "closure-epoch-concurrency-stats returns hash");
    CHECK(epoch_hash(cs, "schema") == 739, "schema == 739");
    CHECK(epoch_hash(cs, "stale-epoch-on-steal") >= 0, "stale-epoch-on-steal present");
    CHECK(epoch_hash(cs, "fence-enforced") >= 0, "fence-enforced present");
    CHECK(epoch_hash(cs, "linear-violation-prevented") >= 0, "linear-violation-prevented present");

    const auto fence0 = epoch_hash(cs, "fence-enforced");
    const auto stale0 = epoch_hash(cs, "stale-epoch-on-steal");

    std::println("\n--- AC2: fence-enforced bumps on epoch-sensitive path ---");
    auto r = cs.eval("(set-code \"(define (mk-adder n) (lambda (x) (+ x n)))"
                     " (define add3 (mk-adder 3))\") "
                     "(eval-current) "
                     "(add3 7)");
    CHECK(r && is_int(*r) && as_int(*r) == 10, "(add3 7) == 10");
    (void)cs.evaluator().current_bridge_epoch();
    const auto fence1 = epoch_hash(cs, "fence-enforced");
    std::println("  fence-enforced: {} -> {}", fence0, fence1);
    CHECK(fence1 > fence0, "fence-enforced grew after acquire epoch read");

    std::println("\n--- AC3: invalidate_function + epoch visibility ---");
    const auto refresh_before = cs.get_closure_stale_refresh_count();
    const auto invalidate_before =
        cs.metrics().invalidate_function_calls.load(std::memory_order_relaxed);
    cs.public_invalidate_function("mk-adder");
    const auto invalidate_after =
        cs.metrics().invalidate_function_calls.load(std::memory_order_relaxed);
    const auto refresh_after = cs.get_closure_stale_refresh_count();
    CHECK(invalidate_after > invalidate_before,
          "invalidate_function_calls grew after public_invalidate_function");
    std::println("  stale-refresh: {} -> {} invalidate_calls: {} -> {}", refresh_before,
                 refresh_after, invalidate_before, invalidate_after);
    CHECK(refresh_after > refresh_before,
          "closure_stale_refresh grew (release epoch bump visible)");
    CHECK(cs.eval("(mutate:rebind \"mk-adder\" \"(lambda (n) (lambda (x) (+ x n)))\" "
                  "\"issue739\")")
              .has_value(),
          "mutate:rebind mk-adder under Guard after invalidate");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after invalidate");

    auto post = cs.eval("(set-code \"(define (mk-adder n) (lambda (x) (+ x n)))"
                        " (define add9 (mk-adder 9))\") "
                        "(eval-current) "
                        "(add9 1)");
    CHECK(post && is_int(*post) && as_int(*post) == 10,
          "post-invalidate apply safe (add9 1) == 10");

    std::println("\n--- AC4: multi-fiber steal/yield + invalidate stress ---");
    const auto stats4a = stats_sum(cs);
    Scheduler sched(4);
    std::atomic<int> done{0};
    constexpr int k_fibers = 12;
    for (int f = 0; f < k_fibers; ++f) {
        sched.spawn_with_affinity(
            [&, f]() {
                std::mt19937 rng(static_cast<unsigned>(739u + f));
                std::uniform_int_distribution<int> coin(0, 2);
                for (int i = 0; i < 12; ++i) {
                    if (coin(rng) == 0) {
                        aura_evaluator_test_push_mutation_checkpoint();
                        Fiber::yield(YieldReason::MutationBoundary);
                        aura_evaluator_test_pop_mutation_checkpoint();
                    } else if (coin(rng) == 1) {
                        Fiber::yield(YieldReason::MutationBoundary);
                    } else {
                        Fiber::yield(YieldReason::Explicit);
                    }
                }
                done.fetch_add(1, std::memory_order_relaxed);
            },
            f % 2);
    }
    std::thread invalidate_worker([&] {
        for (int i = 0; i < 6; ++i) {
            cs.public_invalidate_function("mk-adder");
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    });
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (done.load() < k_fibers && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    sched.stop();
    io.join();
    invalidate_worker.join();
    CHECK(done.load() == k_fibers, "all fibers completed under epoch+steal stress");

    std::println("\n--- AC5: metrics monotonic ---");
    const auto stats4b = stats_sum(cs);
    const auto stale1 = epoch_hash(cs, "stale-epoch-on-steal");
    std::println("  epoch-concurrency sum: {} -> {} stale-steal: {} -> {}", stats4a, stats4b,
                 stale0, stale1);
    CHECK(stats4b >= stats4a, "epoch-concurrency stats monotonic over stress");
    CHECK(epoch_hash(cs, "fence-enforced") > fence0, "fence-enforced grew over full matrix");

    std::println("\n--- AC6: query regression ---");
    auto closure_stats = cs.eval("(query:closure-stats)");
    auto compile_epoch = cs.eval("(compile:epoch)");
    CHECK(closure_stats && is_hash(*closure_stats), "query:closure-stats regression");
    CHECK(compile_epoch && is_int(*compile_epoch), "compile:epoch regression");
}

} // namespace aura_issue_739_detail

int aura_issue_prompt6_epoch_atomic_visibility_fiber_steal_run() {
    aura::compiler::CompilerService cs;
    aura_issue_739_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_prompt6_epoch_atomic_visibility_fiber_steal_run();
}
#endif
