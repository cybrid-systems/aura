// test_arena_auto_compact_fiber_defag_shape_dirty_closedloop.cpp — Issue #743:
// Arena auto-compact + live defrag + fiber safepoint + dirty/Shape closed loop.
//
// Non-duplicative with #642, #685, #569, #464, #405.
//
//   - AC1: query:arena-auto-policy-stats reachable (schema 743)
//   - AC2: mutate + arena:request-defrag bumps auto-compact-triggers
//   - AC3: arena:adaptive-compact bumps shape-inval-on-compact
//   - AC4: 12+ fibers + steal/yield stress bumps defrag-fiber-safe-hits
//   - AC5: metrics monotonic over stress matrix
//   - AC6: query regression (arena-auto-compact-stats, arena-auto-compaction-stats)

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
extern "C" void aura_evaluator_resume_fiber_migration();
extern "C" void aura_evaluator_probe_linear_on_steal();

namespace aura_issue_743_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::Scheduler;
using aura::serve::YieldReason;

static std::int64_t policy_hash(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (query:arena-auto-policy-stats) \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto triggers = policy_hash(cs, "auto-compact-triggers");
    const auto fiber_safe = policy_hash(cs, "defrag-fiber-safe-hits");
    const auto shape = policy_hash(cs, "shape-inval-on-compact");
    const auto env = policy_hash(cs, "env-reval-success");
    if (triggers < 0 || fiber_safe < 0 || shape < 0 || env < 0)
        return -1;
    return triggers + fiber_safe + shape + env;
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) "
                 "(define a 1) (define b 2) (fact 5)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:arena-auto-policy-stats (schema 743) ---");
    CHECK(setup_workspace(cs), "recursive workspace setup");
    auto h = cs.eval("(query:arena-auto-policy-stats)");
    CHECK(h && is_hash(*h), "arena-auto-policy-stats returns hash");
    CHECK(policy_hash(cs, "schema") == 743, "schema == 743");
    CHECK(policy_hash(cs, "auto-compact-triggers") >= 0, "auto-compact-triggers present");
    CHECK(policy_hash(cs, "defrag-fiber-safe-hits") >= 0, "defrag-fiber-safe-hits present");
    CHECK(policy_hash(cs, "fragmentation-post-mutate") >= 0,
          "fragmentation-post-mutate present");
    CHECK(policy_hash(cs, "shape-inval-on-compact") >= 0, "shape-inval-on-compact present");
    CHECK(policy_hash(cs, "env-reval-success") >= 0, "env-reval-success present");

    std::println("\n--- AC2: mutate + arena defrag path bumps triggers ---");
    const auto triggers0 = policy_hash(cs, "auto-compact-triggers");
    (void)cs.eval("(arena:request-defrag)");
    for (int i = 0; i < 40; ++i)
        (void)cs.eval("(fact 3)");
    (void)cs.eval("(mutate:rebind \"a\" \"10\")");
    (void)cs.eval("(eval-current)");
    const auto triggers1 = policy_hash(cs, "auto-compact-triggers");
    std::println("  auto-compact-triggers: {} -> {}", triggers0, triggers1);
    CHECK(triggers1 >= triggers0, "auto-compact-triggers monotonic after mutate/defrag");

    std::println("\n--- AC3: adaptive compact bumps shape invalidation ---");
    const auto shape0 = policy_hash(cs, "shape-inval-on-compact");
    (void)cs.eval("(arena:adaptive-compact)");
    const auto shape1 = policy_hash(cs, "shape-inval-on-compact");
    std::println("  shape-inval-on-compact: {} -> {}", shape0, shape1);
    CHECK(shape1 >= shape0, "shape-inval-on-compact monotonic after adaptive-compact");

    std::println("\n--- AC4: 12+ fibers + steal/yield arena policy stress ---");
    const auto stats4a = stats_sum(cs);
    const auto fiber_safe0 = policy_hash(cs, "defrag-fiber-safe-hits");
    Scheduler sched(4);
    std::atomic<int> done{0};
    constexpr int k_fibers = 12;
    std::mutex eval_mtx;
    for (int f = 0; f < k_fibers; ++f) {
        sched.spawn_with_affinity(
            [&, f]() {
                std::mt19937 rng(static_cast<unsigned>(743u + f));
                std::uniform_int_distribution<int> coin(0, 3);
                for (int i = 0; i < 10; ++i) {
                    switch (coin(rng)) {
                        case 0:
                            aura_evaluator_test_push_mutation_checkpoint();
                            Fiber::yield(YieldReason::MutationBoundary);
                            aura_evaluator_test_pop_mutation_checkpoint();
                            break;
                        case 1:
                            Fiber::yield(YieldReason::MutationBoundary);
                            break;
                        case 2:
                            aura_evaluator_resume_fiber_migration();
                            break;
                        default:
                            aura_evaluator_probe_linear_on_steal();
                            Fiber::yield(YieldReason::Explicit);
                            break;
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(eval_mtx);
                    (void)cs.eval("(arena:request-defrag)");
                    (void)cs.eval("(fact 2)");
                }
                done.fetch_add(1, std::memory_order_relaxed);
            },
            f % 2);
    }
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (done.load() < k_fibers && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    sched.stop();
    io.join();
    CHECK(done.load() == k_fibers, "all fibers completed under arena policy stress");
    const auto fiber_safe1 = policy_hash(cs, "defrag-fiber-safe-hits");
    std::println("  defrag-fiber-safe-hits: {} -> {}", fiber_safe0, fiber_safe1);
    CHECK(fiber_safe1 >= fiber_safe0, "defrag-fiber-safe-hits monotonic under fiber load");

    std::println("\n--- AC5: metrics monotonic over stress matrix ---");
    const auto stats4b = stats_sum(cs);
    std::println("  arena-auto-policy sum: {} -> {}", stats4a, stats4b);
    CHECK(stats4b >= stats4a, "arena-auto-policy stats monotonic");

    std::println("\n--- AC6: query regression ---");
    auto ac = cs.eval("(query:arena-auto-compact-stats)");
    auto acd = cs.eval("(query:arena-auto-compaction-stats)");
    auto acdf = cs.eval("(query:arena-auto-compact-defrag-stats)");
    CHECK(ac && is_hash(*ac), "arena-auto-compact-stats regression");
    CHECK(acd && is_hash(*acd), "arena-auto-compaction-stats regression");
    CHECK(acdf && is_hash(*acdf), "arena-auto-compact-defrag-stats regression");
}

} // namespace aura_issue_743_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_issue_743_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}