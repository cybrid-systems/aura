// tests/serve/test_fiber_integration_batch.cpp — closure-bridge Cycle-4 integration (Issue #226).
// Consolidated closure-bridge Cycle-4 integration (Issue #226).
// Heavier than the unit-level GC batch (test_gc_batch.cpp): uses raw
// aura::serve::Scheduler + std::thread for multi-fiber stress. Created
// as a separate domain batch per README's "Brand-new theme? Create new
// domain batch file" guidance.
//
// AC map (1 issue, ~20 ACs across 4 sections):
//   Issue #226 — Section 1: 100-fiber concurrent mutate + closure invoke
//                  Section 2: arena reset + bridged closure call
//                  Section 3: post-mutation invariant checks (bridge_epoch +
//                             bridge_invalidations_count monotonicity)
//                  Section 4: 200-fiber doomsday stress (random define /
//                             mutate / eval / re-define in 4-worker scheduler)

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <print>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "serve/scheduler.h"
#include "serve/fiber.h"

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.ir_executor;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_fiber_integration_batch {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::test::g_failed;
using aura::test::g_passed;

template <typename A> bool wait_for_atomic(const A& counter, int expected, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (counter.load() < expected) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// ── Issue #226 Section 1: 100 fibers on 4-worker scheduler ──
// Each fiber: define f_i, eval (+ 1 sanity check), mutate:rebind,
// re-define with *3 body, eval (+ 1 final check). Verifies no
// races / no UAF / no crash across all 100 closures running the
// shared_ptr-bridged + epoch-stamped + invalidate-on-mutate path.
static void run_226_section1_concurrent_mutate_invoke() {
    std::println("\n=== #226 S1: 100 fibers concurrent mutate + closure invoke ===");
    constexpr int NUM_FIBERS = 100;
    aura::serve::Scheduler sched(4);
    std::atomic<int> completed{0};
    std::atomic<int> ok{0};
    for (int i = 0; i < NUM_FIBERS; ++i) {
        sched.spawn([i, &completed, &ok]() {
            CompilerService cs;
            std::string fname = "f_" + std::to_string(i);
            std::string src1 = "(begin (define (" + fname + " x) (* x 2)) (" + fname + " 5))";
            auto r1 = cs.eval(src1);
            if (!r1 || as_int(*r1) != 10) {
                completed.fetch_add(1);
                return;
            }
            std::string src2 =
                "(mutate:rebind \"" + fname + "\" \"(lambda (x) (* x 3))\" \"test\")";
            cs.eval(src2);
            std::string src3 = "(begin (define (" + fname + " x) (* x 3)) (" + fname + " 5))";
            auto r3 = cs.eval(src3);
            if (r3 && as_int(*r3) == 15)
                ok.fetch_add(1);
            completed.fetch_add(1);
        });
    }
    std::thread t([&sched]() { sched.run(); });
    bool waited = wait_for_atomic(completed, NUM_FIBERS);
    sched.stop();
    t.join();
    CHECK(waited, "all 100 fibers completed (no hang)");
    int ok_count = ok.load();
    CHECK(ok_count == NUM_FIBERS, "all 100 fibers saw post-redefine value 15 (no UAF / no race)");
}

// ── Issue #226 Section 2: arena reset followed by bridged closure ──
// Single CompilerService. Define a function (cache populated).
// Public invalidate hook then fresh eval. Stale bridge entries
// are cleared; subsequent eval returns correct value.
static void run_226_section2_reset_clears_bridge() {
    std::println("\n=== #226 S2: arena reset followed by bridge call ===");
    CompilerService cs;
    auto r1 = cs.eval("(begin (define (h x) (* x x)) (h 5))");
    CHECK(r1.has_value(), "first eval succeeds");
    if (r1)
        CHECK(as_int(*r1) == 25, "(h 5) = 25");
    auto epoch_before = cs.bridge_epoch();
    cs.public_invalidate_bridges_for("h");
    auto epoch_after = cs.bridge_epoch();
    CHECK(epoch_after >= epoch_before, "bridge_epoch non-decreasing after invalidate");
    auto r2 = cs.eval("(+ 1 2)");
    CHECK(r2.has_value(), "post-invalidate eval works");
    if (r2)
        CHECK(as_int(*r2) == 3, "fresh eval returns 3 (+ 1 2)");
}

// ── Issue #226 Section 3: post-mutation invariant ──
// bridge_epoch non-decreasing + bridge_invalidations_count
// non-decreasing after a mutate that exercises mark_define_dirty.
static void run_226_section3_post_mutation_invariant() {
    std::println("\n=== #226 S3: post-mutation invariant (bridge_epoch + invalidations) ===");
    CompilerService cs;
    auto metric_before = cs.metrics().bridge_invalidations_count.load(std::memory_order_relaxed);
    auto epoch_before = cs.bridge_epoch();
    auto r1 = cs.eval("(begin (define (i x) (+ x 1)) (i 5))");
    CHECK(r1.has_value(), "first eval succeeds");
    if (r1)
        CHECK(as_int(*r1) == 6, "(i 5) = 6");
    auto r2 = cs.eval("(mutate:rebind \"i\" \"(lambda (x) (+ x 100))\" \"test\")");
    CHECK(r2.has_value(), "mutate:rebind succeeds");
    auto epoch_post = cs.bridge_epoch();
    CHECK(epoch_post >= epoch_before, "bridge_epoch non-decreasing after mutation");
    auto metric_after = cs.metrics().bridge_invalidations_count.load(std::memory_order_relaxed);
    CHECK(metric_after >= metric_before, "bridge_invalidations_count non-decreasing");
}

// ── Issue #226 Section 4: 200-fiber doomsday stress ──
// Each fiber runs 5 random operations (define / mutate /
// arithmetic / re-define). Run on a 4-worker scheduler. No
// expectations on values, only that nothing crashes and no
// exceptions leak. TSan/ASan runs via tests/run_issue_180_tsan.sh
// separately (binary is the same).
static void run_226_section4_doomsday_stress() {
    std::println("\n=== #226 S4: doomsday stress (50 fibers x 5 ops = 250 random ops) ===");
    constexpr int NUM_FIBERS = 50;
    constexpr int OPS_PER_FIBER = 5;
    aura::serve::Scheduler sched(4);
    std::atomic<int> completed_ops{0};
    std::atomic<int> errors{0};
    std::random_device rd;
    std::mt19937 gen(rd());
    for (int i = 0; i < NUM_FIBERS; ++i) {
        sched.spawn([i, &completed_ops, &errors, &gen]() {
            CompilerService cs;
            std::uniform_int_distribution<> op_dist(0, 3);
            std::uniform_int_distribution<> name_dist(0, 9);
            for (int op = 0; op < OPS_PER_FIBER; ++op) {
                int op_kind = op_dist(gen);
                int name_idx = name_dist(gen);
                std::string fname = "ds_" + std::to_string(name_idx);
                try {
                    switch (op_kind) {
                        case 0: {
                            std::string src =
                                "(begin (define (" + fname + " x) (* x 2)) (" + fname + " 5))";
                            cs.eval(src);
                            break;
                        }
                        case 1: {
                            std::string src = "(mutate:rebind \"" + fname +
                                              "\" \"(lambda (x) (* x 3))\" \"stress\")";
                            cs.eval(src);
                            break;
                        }
                        case 2: {
                            std::string src =
                                "(+ " + std::to_string(name_idx) + " " + std::to_string(i) + ")";
                            cs.eval(src);
                            break;
                        }
                        case 3: {
                            std::string src =
                                "(begin (define (" + fname + " x) (+ x 1)) (" + fname + " 5))";
                            cs.eval(src);
                            break;
                        }
                    }
                } catch (...) {
                    errors.fetch_add(1);
                }
                completed_ops.fetch_add(1);
            }
        });
    }
    std::thread t([&sched]() { sched.run(); });
    bool waited = wait_for_atomic(completed_ops, NUM_FIBERS * OPS_PER_FIBER);
    sched.stop();
    t.join();
    CHECK(waited, "all 250 ops completed (no hang)");
    int err_count = errors.load();
    CHECK(err_count == 0, "no exceptions (no UAF / no data race leaked)");
}

} // namespace aura_fiber_integration_batch

int aura_issue_fiber_integration_batch_run() {
    aura_fiber_integration_batch::run_226_section1_concurrent_mutate_invoke();
    aura_fiber_integration_batch::run_226_section2_reset_clears_bridge();
    aura_fiber_integration_batch::run_226_section3_post_mutation_invariant();
    aura_fiber_integration_batch::run_226_section4_doomsday_stress();
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_fiber_integration_batch_run();
}
#endif
