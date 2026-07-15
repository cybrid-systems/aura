// test_mutate_cross_thread_migration.cpp — Issue #1373:
// MutationBoundaryGuard hold-time + cross-fiber yield/migration counters.

#include "test_harness.hpp"
#include "observability_metrics.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

namespace {

std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:mutation-boundary-hold-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    // ── hold-time on outermost Guard ──
    {
        CompilerService cs;
        CompilerMetrics metrics;
        cs.evaluator().set_compiler_metrics(&metrics);
        const auto holds0 = metrics.mutation_boundary_holds_total.load();
        const auto time0 = metrics.mutation_boundary_hold_time_total_us.load();
        {
            bool ok = true;
            Evaluator::MutationBoundaryGuard guard(cs.evaluator(), &ok);
            CHECK(cs.evaluator().mutation_boundary_held(), "held during Guard");
            CHECK(cs.evaluator().any_active_mutation_boundary(), "depth > 0");
            // Spin briefly so hold_us can be non-zero
            auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::microseconds(50)) {
            }
        }
        CHECK(!cs.evaluator().mutation_boundary_held(), "cleared after Guard dtor");
        CHECK(metrics.mutation_boundary_holds_total.load() == holds0 + 1, "holds_total +1");
        CHECK(metrics.mutation_boundary_hold_time_total_us.load() >= time0,
              "hold_time non-decreasing");
        CHECK(metrics.mutation_hold_samples.load() >= 1, "#1253 samples also bumped");
        cs.evaluator().set_compiler_metrics(nullptr);
    }

    // ── nested Guard: only outermost records hold sample ──
    {
        CompilerService cs;
        CompilerMetrics metrics;
        cs.evaluator().set_compiler_metrics(&metrics);
        const auto holds0 = metrics.mutation_boundary_holds_total.load();
        {
            bool ok = true;
            Evaluator::MutationBoundaryGuard outer(cs.evaluator(), &ok);
            {
                bool ok2 = true;
                Evaluator::MutationBoundaryGuard inner(cs.evaluator(), &ok2);
                CHECK(cs.evaluator().mutation_boundary_held(), "still held nested");
            }
            CHECK(cs.evaluator().mutation_boundary_held(), "held until outer ends");
        }
        CHECK(metrics.mutation_boundary_holds_total.load() == holds0 + 1,
              "only outermost hold sample");
        cs.evaluator().set_compiler_metrics(nullptr);
    }

    // ── query:mutation-boundary-hold-stats ──
    {
        CompilerService cs;
        CompilerMetrics metrics;
        cs.evaluator().set_compiler_metrics(&metrics);
        {
            bool ok = true;
            Evaluator::MutationBoundaryGuard guard(cs.evaluator(), &ok);
        }
        auto r = cs.eval("(engine:metrics \"query:mutation-boundary-hold-stats\")");
        CHECK(r && is_hash(*r), "hold-stats is hash");
        CHECK(href(cs, "holds-total") >= 1, "holds-total key");
        CHECK(href(cs, "hold-time-us-total") >= 0, "hold-time-us-total key");
        CHECK(href(cs, "same-thread-yield") >= 0, "same-thread-yield key");
        CHECK(href(cs, "cross-thread-migration") >= 0, "cross-thread-migration key");
        CHECK(href(cs, "yield-rollback") >= 0, "yield-rollback key");
        CHECK(href(cs, "holds-over-1ms") >= 0, "holds-over-1ms key");
        CHECK(href(cs, "avg-hold-us") >= 0, "avg-hold-us key");
        CHECK(href(cs, "held-now") == 0, "held-now 0 after dtor");
        // Schema bumped to 1375 when hold histogram shipped (#1375).
        CHECK(href(cs, "schema") == 1375 || href(cs, "schema") == 1373, "schema 1375");
        cs.evaluator().set_compiler_metrics(nullptr);
    }

    // ── same-thread yield checkpoint ──
    {
        CompilerService cs;
        CompilerMetrics metrics;
        cs.evaluator().set_compiler_metrics(&metrics);
        const auto y0 = metrics.mutation_boundary_yield_same_thread_total.load();
        {
            bool ok = true;
            Evaluator::MutationBoundaryGuard guard(cs.evaluator(), &ok);
            cs.evaluator().checkpoint_yield_boundary(true);
            const bool restored = cs.evaluator().restore_post_yield_or_rollback();
            CHECK(restored, "same-thread restore ok");
        }
        CHECK(metrics.mutation_boundary_yield_same_thread_total.load() == y0 + 1,
              "same-thread yield +1");
        CHECK(cs.evaluator().mutation_yield_count() >= 1, "mutation_yield_count bumped");
        cs.evaluator().set_compiler_metrics(nullptr);
    }

    // ── multi-thread: concurrent Guard on separate Evaluators (no shared mtx) ──
    {
        std::atomic<int> held_seen{0};
        std::atomic<int> holds_sum{0};
        std::atomic<int> errors{0};
        constexpr int kThreads = 4;
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&]() {
                try {
                    CompilerService cs;
                    CompilerMetrics metrics;
                    cs.evaluator().set_compiler_metrics(&metrics);
                    {
                        bool ok = true;
                        Evaluator::MutationBoundaryGuard guard(cs.evaluator(), &ok);
                        if (cs.evaluator().mutation_boundary_held())
                            held_seen.fetch_add(1, std::memory_order_relaxed);
                        // Concurrent sibling threads run their own Guards
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                    holds_sum.fetch_add(
                        static_cast<int>(metrics.mutation_boundary_holds_total.load()),
                        std::memory_order_relaxed);
                    cs.evaluator().set_compiler_metrics(nullptr);
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads)
            th.join();
        CHECK(errors.load() == 0, "no exceptions in concurrent Guards");
        CHECK(held_seen.load() == kThreads, "each thread saw held flag");
        CHECK(holds_sum.load() == kThreads, "each Guard recorded one hold");
    }

    // ── migration probe: yield checkpoint stamped with foreign thread_id ──
    // Yield stacks are thread_local when no Fiber is active, so we inject a
    // checkpoint with a non-current thread_id and restore on this thread
    // (simulates steal/resume on a different worker).
    {
        CompilerService cs;
        CompilerMetrics metrics;
        cs.evaluator().set_compiler_metrics(&metrics);
        const auto mig0 = metrics.mutation_boundary_cross_thread_migration_total.load();
        {
            bool ok = true;
            Evaluator::MutationBoundaryGuard guard(cs.evaluator(), &ok);
            Evaluator::YieldBoundaryCheckpoint cp;
            cp.defuse_version = cs.evaluator().get_defuse_version();
            cp.boundary_depth = Evaluator::mutation_boundary_depth();
            cp.mutation_stack_depth = 0;
            cp.thread_id = std::thread::id{}; // not-a-thread ≠ current
            cp.had_active_boundary = true;
            cs.evaluator().active_yield_checkpoint_stack().push_back(cp);
            (void)cs.evaluator().restore_post_yield_or_rollback();
        }
        CHECK(metrics.mutation_boundary_cross_thread_migration_total.load() >= mig0 + 1,
              "cross-thread migration +1");
        // Guard dtor still runs — no UAF
        CHECK(!cs.evaluator().mutation_boundary_held(), "cleared after migration probe");
        cs.evaluator().set_compiler_metrics(nullptr);
    }

    // ── Guard dtor after forced rollback flag ──
    {
        CompilerService cs;
        CompilerMetrics metrics;
        cs.evaluator().set_compiler_metrics(&metrics);
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard guard(cs.evaluator(), &ok);
            ok = false; // force rollback path in dtor
        }
        CHECK(!cs.evaluator().mutation_boundary_held(), "cleared after failed guard");
        CHECK(metrics.mutation_boundary_holds_total.load() >= 1, "hold still counted on fail");
        cs.evaluator().set_compiler_metrics(nullptr);
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("mutate cross-thread migration #1373: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
