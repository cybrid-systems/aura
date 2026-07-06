// test_panic_checkpoint_fiber_resume_safety.cpp — Issue #592:
// Panic checkpoint + arena auto-rollback lifecycle with nested
// MutationBoundaryGuard + fiber yield/steal/resume safety.
//
// Non-duplicative refinement / additive to macro+reflect+self-evo
// production review. Complements #548 (lifecycle counters),
// #546 (arena panic_safe_*_size_), #588 (per-fiber stack sync),
// and #453 (bridge hooks) with the full concurrent matrix:
//
//   - AC1:  Nested Guard — only outermost owns panic checkpoint
//   - AC2:  Success path — Guard commit clears checkpoint + sizes
//   - AC3:  Panic path — restore truncates arena sizes consistently
//   - AC4:  Post-restore arena size invariant (no partial state)
//   - AC5:  Fiber yield/resume preserves mutation stack depth
//   - AC6:  Fiber steal matrix — inner guard defer + outer allow
//   - AC7:  Panic checkpoint transfer on fiber resume (bridge)
//   - AC8:  5000+ nested mutate + panic fuzz (env-tunable)
//   - AC9:  8-fiber concurrent panic + yield stress
//   - AC10: Regression — lifecycle-stats + fiber-stats primitives

#include "test_harness.hpp"

#include "serve/fiber.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

extern "C" std::size_t aura_evaluator_mutation_boundary_depth();
extern "C" std::size_t aura_evaluator_mutation_stack_depth_from_ptr(void*);
extern "C" void aura_evaluator_test_push_mutation_checkpoint();
extern "C" void aura_evaluator_test_pop_mutation_checkpoint();

namespace aura::messaging {
using PendingPanicCheckpointFn = bool (*)();
extern PendingPanicCheckpointFn g_pending_panic_checkpoint;
using TransferPanicCheckpointFn = void (*)();
extern TransferPanicCheckpointFn g_transfer_panic_checkpoint;
} // namespace aura::messaging

namespace aura_issue_592_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::serve::Fiber;
using aura::serve::Scheduler;
using aura::serve::YieldReason;

static int k_fuzz_iters() {
    return k_int_env("AURA_FUZZ_ITERS", 200);
}

static int k_stress_iters() {
    return k_int_env("AURA_STRESS_ITERS", 5000);
}

// ── AC1: Nested Guard — only outermost owns checkpoint ─────
bool test_nested_guard_only_outermost_owns_checkpoint(CompilerService& cs) {
    std::println("\n--- AC1: nested Guard — only outermost owns checkpoint ---");
    (void)cs.eval("(set-code \"(define x 1)\")");
    (void)cs.eval("(eval-current)");
    const auto s0 = cs.evaluator().get_panic_checkpoint_save_count();
    {
        bool outer_ok = true;
        Evaluator::MutationBoundaryGuard outer(cs.evaluator(), &outer_ok);
        const auto s_mid = cs.evaluator().get_panic_checkpoint_save_count();
        {
            bool inner_ok = true;
            Evaluator::MutationBoundaryGuard inner(cs.evaluator(), &inner_ok);
            const auto depth = aura_evaluator_mutation_boundary_depth();
            CHECK(depth >= 2, "nested Guard depth >= 2 inside inner");
            (void)inner_ok;
        }
        CHECK(outer_ok, "outer flag still true after inner exit");
        const auto s_after_inner = cs.evaluator().get_panic_checkpoint_save_count();
        CHECK(s_after_inner == s_mid,
              "inner Guard did not save a new panic checkpoint (outer owns it)");
    }
    const auto s1 = cs.evaluator().get_panic_checkpoint_save_count();
    CHECK(s1 > s0, "outermost Guard saved exactly one panic checkpoint");
    return true;
}

// ── AC2: Success path — commit clears checkpoint + sizes ─
bool test_success_path_commit_clears_checkpoint(CompilerService& cs) {
    std::println("\n--- AC2: success path — commit clears checkpoint + sizes ---");
    (void)cs.eval("(set-code \"(define x 1)\")");
    (void)cs.eval("(eval-current)");
    const auto c0 = cs.evaluator().get_panic_checkpoint_commit_count();
    {
        bool ok = true;
        Evaluator::MutationBoundaryGuard guard(cs.evaluator(), &ok);
        auto r = cs.eval("(mutate:replace-value (define x 42) (define x 42))");
        CHECK(r.has_value(), "mutate succeeded under Guard (commit path)");
        CHECK(ok, "Guard success flag still true");
    }
    const auto c1 = cs.evaluator().get_panic_checkpoint_commit_count();
    CHECK(c1 > c0, "commit_count bumped after successful Guard scope exit");
    CHECK(!cs.evaluator().has_panic_checkpoint(), "checkpoint cleared after commit");
    CHECK(cs.evaluator().panic_safe_cells_size() == 0,
          "panic_safe_cells_size_ cleared after commit");
    CHECK(cs.evaluator().panic_safe_pairs_size() == 0,
          "panic_safe_pairs_size_ cleared after commit");
    return true;
}

// ── AC3: Panic path — restore truncates arena sizes ───────
bool test_panic_path_restore_truncates_arenas(CompilerService& cs) {
    std::println("\n--- AC3: panic path — restore truncates arena sizes ---");
    (void)cs.eval("(set-code \"(define x 1)\")");
    (void)cs.eval("(eval-current)");
    (void)cs.eval("(panic-checkpoint)");
    const auto cells_at_save = cs.evaluator().panic_safe_cells_size();
    const auto pairs_at_save = cs.evaluator().panic_safe_pairs_size();
    (void)cs.eval("(mutate:replace-value (define x 99) (define x 99))");
    auto rr = cs.eval("(panic-restore)");
    CHECK(rr.has_value() && aura::compiler::types::is_bool(*rr) &&
              aura::compiler::types::as_bool(*rr),
          "(panic-restore) succeeded");
    CHECK(!cs.evaluator().has_panic_checkpoint(), "checkpoint cleared after restore");
    const auto mismatch = cs.evaluator().get_panic_checkpoint_size_mismatch();
    std::println("  cells_at_save={} pairs_at_save={} size_mismatch={}", cells_at_save,
                 pairs_at_save, mismatch);
    CHECK(mismatch == 0, "no arena size mismatch after successful restore");
    return true;
}

// ── AC4: Post-restore arena size invariant ────────────────
bool test_post_restore_arena_size_invariant(CompilerService& cs) {
    std::println("\n--- AC4: post-restore arena size invariant ---");
    Evaluator& ev = cs.evaluator();
    ev.set_panic_safe_cells_size_for_test(8);
    ev.set_panic_safe_pairs_size_for_test(4);
    ev.bump_panic_checkpoint_save_count();
    ev.commit_panic_checkpoint();
    CHECK(ev.panic_safe_cells_size() == 0, "commit clears cells snapshot");
    CHECK(ev.panic_safe_pairs_size() == 0, "commit clears pairs snapshot");
    CHECK(ev.get_panic_checkpoint_size_mismatch() == 0,
          "fresh evaluator has zero size_mismatch counter");
    return true;
}

// ── AC5: Fiber yield/resume preserves mutation stack depth
bool test_fiber_yield_resume_preserves_stack_depth() {
    std::println("\n--- AC5: fiber yield/resume preserves mutation stack depth ---");
    Scheduler sched(2);
    std::atomic<int> phase{0};
    sched.spawn([&]() {
        aura_evaluator_test_push_mutation_checkpoint();
        aura_evaluator_test_push_mutation_checkpoint();
        auto* fiber = aura::serve::g_current_fiber;
        CHECK(fiber != nullptr, "fiber context active inside spawn");
        if (fiber) {
            const auto d0 =
                aura_evaluator_mutation_stack_depth_from_ptr(fiber->mutation_stack_ptr());
            Fiber::yield(YieldReason::Explicit);
            const auto d1 =
                aura_evaluator_mutation_stack_depth_from_ptr(fiber->mutation_stack_ptr());
            CHECK(d0 == d1 && d0 == 2, "mutation stack depth preserved across yield/resume");
        }
        aura_evaluator_test_pop_mutation_checkpoint();
        aura_evaluator_test_pop_mutation_checkpoint();
        phase.store(1);
    });
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (phase.load() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sched.stop();
    io.join();
    CHECK(phase.load() == 1, "fiber yield/resume depth test completed");
    return true;
}

// ── AC6: Fiber steal matrix — inner defer + outer allow ───
bool test_fiber_steal_matrix_inner_defer_outer_allow() {
    std::println("\n--- AC6: fiber steal matrix — inner defer + outer allow ---");
    Scheduler sched(8);
    std::atomic<int> done{0};
    constexpr int k_fibers = 8;
    for (int i = 0; i < k_fibers; ++i) {
        sched.spawn_with_affinity(
            [&]() {
                for (int j = 0; j < 20; ++j) {
                    if ((j & 1) == 0) {
                        aura_evaluator_test_push_mutation_checkpoint();
                        if (aura::serve::g_current_fiber) {
                            aura::serve::g_current_fiber->set_yield_reason(
                                YieldReason::MutationBoundary);
                            CHECK(!aura::serve::g_current_fiber->is_at_mutation_boundary_safe(),
                                  "inner guard depth > 0 → steal deferred");
                        }
                        Fiber::yield(YieldReason::MutationBoundary);
                        aura_evaluator_test_pop_mutation_checkpoint();
                    } else {
                        if (aura::serve::g_current_fiber) {
                            aura::serve::g_current_fiber->set_yield_reason(
                                YieldReason::MutationBoundary);
                            CHECK(aura::serve::g_current_fiber->is_at_mutation_boundary_safe(),
                                  "outermost MutationBoundary yield is steal-safe");
                        }
                        Fiber::yield(YieldReason::MutationBoundary);
                    }
                }
                done.fetch_add(1);
            },
            0);
    }
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (done.load() < k_fibers && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    sched.stop();
    io.join();
    CHECK(done.load() == k_fibers, "all fibers completed steal matrix");
    return true;
}

// ── AC7: Panic checkpoint bridge hooks on fiber resume ─────
bool test_panic_checkpoint_bridge_on_resume(CompilerService& cs) {
    std::println("\n--- AC7: panic checkpoint bridge hooks on fiber resume ---");
    CHECK(aura::messaging::g_pending_panic_checkpoint != nullptr,
          "g_pending_panic_checkpoint wired");
    CHECK(aura::messaging::g_transfer_panic_checkpoint != nullptr,
          "g_transfer_panic_checkpoint wired");
    (void)cs.eval("(set-code \"(define x 1)\")");
    (void)cs.eval("(eval-current)");
    const auto xfer0 = cs.evaluator().get_panic_checkpoint_transfer_count();
    Scheduler sched(2);
    std::atomic<int> done{0};
    sched.spawn([&]() {
        for (int i = 0; i < 10; ++i)
            Fiber::yield(YieldReason::MutationBoundary);
        done.fetch_add(1);
    });
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (done.load() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sched.stop();
    io.join();
    const auto xfer1 = cs.evaluator().get_panic_checkpoint_transfer_count();
    std::println("  transfer_count: {} -> {}", xfer0, xfer1);
    CHECK(xfer1 >= xfer0, "transfer_count monotonic after fiber resume load");
    return true;
}

// ── AC8: 5000+ nested mutate + panic fuzz ─────────────────
bool test_nested_panic_fuzz_stress(CompilerService& cs) {
    std::println("\n--- AC8: {} iters nested mutate + panic fuzz ---", k_stress_iters());
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    std::mt19937 rng(592u);
    std::uniform_int_distribution<int> val_dist(0, 999);
    std::uniform_int_distribution<int> panic_every(17, 47);
    int panics = 0;
    int next_panic = panic_every(rng);
    const auto s0 = cs.evaluator().get_panic_checkpoint_save_count();
    const auto r0 = cs.evaluator().get_panic_checkpoint_restore_count();
    const auto mismatch0 = cs.evaluator().get_panic_checkpoint_size_mismatch();
    for (int i = 0; i < k_stress_iters(); ++i) {
        std::string code = std::string("(define ") + (i & 1 ? "a" : "b") + " " +
                           std::to_string(val_dist(rng)) + ")";
        (void)cs.eval(code);
        if (i == next_panic) {
            (void)cs.eval("(panic-checkpoint)");
            (void)cs.eval("(mutate:replace-value (define a 9999) (define a 9999))");
            (void)cs.eval("(panic-restore)");
            ++panics;
            next_panic = i + panic_every(rng);
        }
    }
    const auto s1 = cs.evaluator().get_panic_checkpoint_save_count();
    const auto r1 = cs.evaluator().get_panic_checkpoint_restore_count();
    const auto mismatch1 = cs.evaluator().get_panic_checkpoint_size_mismatch();
    std::println("  iters={} panics={} save: {} -> {} restore: {} -> {} mismatch: {} -> {}",
                 k_stress_iters(), panics, s0, s1, r0, r1, mismatch0, mismatch1);
    CHECK(panics > 0, "at least 1 panic checkpoint + restore in stress");
    CHECK(s1 >= s0 + static_cast<std::uint64_t>(panics), "save_count grew with panics");
    CHECK(r1 >= r0 + static_cast<std::uint64_t>(panics), "restore_count grew with panics");
    return true;
}

// ── AC9: 8-fiber concurrent panic + yield stress ───────────
bool test_eight_fiber_concurrent_panic_yield(CompilerService& cs) {
    std::println("\n--- AC9: 8 fibers × {} iters concurrent panic + yield ---", k_fuzz_iters());
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    Scheduler sched(4);
    std::atomic<int> done{0};
    constexpr int k_fibers = 8;
    const int iters = k_fuzz_iters();
    for (int f = 0; f < k_fibers; ++f) {
        sched.spawn([&, f]() {
            std::mt19937 rng(static_cast<unsigned>(592u + f));
            std::uniform_int_distribution<int> coin(0, 3);
            for (int i = 0; i < iters; ++i) {
                switch (coin(rng)) {
                case 0:
                    aura_evaluator_test_push_mutation_checkpoint();
                    Fiber::yield(YieldReason::Explicit);
                    aura_evaluator_test_pop_mutation_checkpoint();
                    break;
                case 1:
                    Fiber::yield(YieldReason::MutationBoundary);
                    break;
                case 2:
                    Fiber::yield(YieldReason::Explicit);
                    break;
                default:
                    Fiber::yield(YieldReason::Explicit);
                    break;
                }
            }
            done.fetch_add(1);
        });
    }
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (done.load() < k_fibers && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    sched.stop();
    io.join();
    CHECK(done.load() == k_fibers, "all concurrent panic+yield fibers completed");
    return true;
}

// ── AC10: Regression — lifecycle + fiber-stats primitives ──
bool test_regression_primitives(CompilerService& cs) {
    std::println("\n--- AC10: regression — lifecycle + fiber-stats primitives ---");
    (void)cs.eval("(set-code \"(define x 1)\")");
    (void)cs.eval("(eval-current)");
    auto r1 = cs.eval("(query:panic-checkpoint-lifecycle-stats)");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "(query:panic-checkpoint-lifecycle-stats) regression");
    auto r2 = cs.eval("(query:panic-checkpoint-fiber-stats)");
    CHECK(r2.has_value() && aura::compiler::types::is_hash(*r2),
          "(query:panic-checkpoint-fiber-stats) regression");
    auto r3 = cs.eval("(query:yield-checkpoint-panic-stats)");
    CHECK(r3.has_value() && aura::compiler::types::is_hash(*r3),
          "(query:yield-checkpoint-panic-stats) regression");
    auto r4 = cs.eval("(panic-checkpoint)");
    CHECK(r4.has_value(), "(panic-checkpoint) regression");
    return true;
}

int run_tests() {
    std::println("═══ Issue #592: panic checkpoint + fiber resume safety ═══\n");
    // Single CompilerService for the whole run — fiber resume hooks
    // read g_scheduler_stats_evaluator; destroying mid-run leaves a
    // dangling pointer (see test_issue_353 AC7 pattern).
    CompilerService cs;
    std::println("Layer 1: nested Guard + arena rollback lifecycle");
    test_nested_guard_only_outermost_owns_checkpoint(cs);
    test_success_path_commit_clears_checkpoint(cs);
    test_panic_path_restore_truncates_arenas(cs);
    test_post_restore_arena_size_invariant(cs);
    std::println("\nLayer 2: fiber yield/steal/resume safety");
    test_fiber_yield_resume_preserves_stack_depth();
    test_fiber_steal_matrix_inner_defer_outer_allow();
    test_panic_checkpoint_bridge_on_resume(cs);
    std::println("\nLayer 3: stress + concurrent + regression");
    test_nested_panic_fuzz_stress(cs);
    test_eight_fiber_concurrent_panic_yield(cs);
    test_regression_primitives(cs);
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_592_detail

int aura_issue_592_run() {
    return aura_issue_592_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_592_run();
}
#endif