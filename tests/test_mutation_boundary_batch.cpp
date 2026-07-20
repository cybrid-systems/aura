// test_mutation_boundary_batch.cpp
// B pilot #10 (after dead_coercion in 25d205e6): consolidated mutation_boundary
// family — Issues #1591 + #1444 + #417 + #548 (safe-yield fairness +
// full coverage audit + invariant closed loop + panic rollback fiber resume)
// into one batch driver.
//
// Per AuraDomainTests.cmake legacy Phase 1 batch convention (per_defuse_batch /
// env_lookup_batch / fiber_resume_batch / compact_sweep_batch /
// incremental_relower_batch / macro_reflect_batch / incremental_type_batch /
// linear_ownership_batch / dead_coercion_batch precedents): single binary with
// CHECK() + RUN_ALL_TESTS(); per-issue AC blocks in namespace
// aura_mutation_boundary_batch { run_NNN_xxx() }; EXCLUDE_FROM_ALL.
//
// AC map (consolidated, 29 ACs total):
//   Issue #1591 — 6 ACs: safe-yield schema 1591 + per-fiber depth +
//                  fairness dashboard + yield-vs-held + steal metric +
//                  4-thread concurrent probes
//   Issue #1444 — 5 ACs: coverage primitive audit + naked-mutate counter
//                  + set!/set-car!/set-cdr! wrapped + basic mutate cycle
//   Issue #417  — 7 ACs: invariant stats + mutate bumps epoch +
//                  nested enter/exit + ensure_mutation_invariants +
//                  eval-current probe + multi-round matrix +
//                  mutation-coord / fiber-migration regression
//   Issue #548  — 11 ACs: panic-checkpoint 4 lifecycle counters +
//                   (query:panic-checkpoint-lifecycle-stats) +
//                   nested Guard + panic-at-depth-0 + restore lifecycle +
//                   commit lifecycle + per-fiber depth probe +
//                   100+ iters fuzz + gc-heap + 8-thread concurrent +
//                   regression

#include "test_harness.hpp"
#include "serve/metrics.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <print>
#include <random>
#include <string>
#include <thread>
#include <vector>

// No-op shim: original #1444 (mutation_boundary_full_coverage.cpp) AC helpers
// used TEST_LOG + static bool return. Converted to void/CHECK() here, so the
// inline logging calls are dead code — define as no-op for diff parity with
// the original source (no #include required).
#define TEST_LOG(msg)                                                                              \
    do {                                                                                           \
        (void)(msg);                                                                               \
    } while (0)

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

// C-linkage shim from evaluator_fiber_mutation.cpp
extern "C" std::size_t aura_evaluator_mutation_boundary_depth();

namespace aura_mutation_boundary_batch {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t href(CompilerService& cs, const char* prim, const char* key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", prim, key));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

static int k_fuzz_iters() {
    return k_int_env("AURA_FUZZ_ITERS", 100);
}

// ---------------------------------------------------------------------------
// Issue #1591: mutation-boundary safe-yield + per-fiber depth + fairness
//               (6 ACs)
// ---------------------------------------------------------------------------
namespace _1591_detail {

    static void ac1_safe_yield_schema(CompilerService& cs) {
        std::println("\n--- AC1: safe-yield schema 1591 ---");
        auto h = cs.eval("(engine:metrics \"query:mutation-boundary-safe-yield\")");
        CHECK(h && is_hash(*h), "safe-yield returns hash");
        CHECK(href(cs, "query:mutation-boundary-safe-yield", "schema") == 1635 ||
                  href(cs, "query:mutation-boundary-safe-yield", "schema") == 1591,
              "schema 1635|1591");
        CHECK(href(cs, "query:mutation-boundary-safe-yield", "issue") == 1635 ||
                  href(cs, "query:mutation-boundary-safe-yield", "issue") == 1591,
              "issue 1635|1591");
        CHECK(href(cs, "query:mutation-boundary-safe-yield", "boundary-depth") >= 0, "depth");
        CHECK(href(cs, "query:mutation-boundary-safe-yield", "avg-hold-time-us") >= 0, "avg hold");
        CHECK(href(cs, "query:mutation-boundary-safe-yield",
                   "steal-inner-deferred-starvation-mitigated-count") >= 0,
              "steal mitigation field");
        auto st = cs.eval("(engine:metrics \"query:mutation-boundary-safe-yield-stats\")");
        CHECK(st && is_hash(*st), "stats hash");
        CHECK(href(cs, "query:mutation-boundary-safe-yield-stats", "schema") == 1635 ||
                  href(cs, "query:mutation-boundary-safe-yield-stats", "schema") == 1591,
              "stats schema 1635|1591");
    }

    static void ac2_depth_stats(CompilerService& cs) {
        std::println("\n--- AC2: per-fiber-mutation-depth-stats ---");
        auto h = cs.eval("(engine:metrics \"query:per-fiber-mutation-depth-stats\")");
        CHECK(h && is_hash(*h), "depth-stats hash");
        CHECK(href(cs, "query:per-fiber-mutation-depth-stats", "schema") == 1635 ||
                  href(cs, "query:per-fiber-mutation-depth-stats", "schema") == 1591,
              "schema 1635|1591");
        CHECK(href(cs, "query:per-fiber-mutation-depth-stats", "lifetime-max") >= 0,
              "lifetime-max");
        CHECK(href(cs, "query:per-fiber-mutation-depth-stats", "live-depth") >= 0, "live-depth");
        CHECK(href(cs, "query:per-fiber-mutation-depth-stats",
                   "safepoint-wait-while-mutation-held-us") >= 0,
              "safepoint wait field");
    }

    static void ac3_fairness_dashboard(CompilerService& cs) {
        std::println("\n--- AC3: mutation-boundary-fairness-stats ---");
        auto h = cs.eval("(engine:metrics \"query:mutation-boundary-fairness-stats\")");
        CHECK(h && is_hash(*h), "fairness hash");
        CHECK(href(cs, "query:mutation-boundary-fairness-stats", "schema") == 1635 ||
                  href(cs, "query:mutation-boundary-fairness-stats", "schema") == 1591,
              "schema 1635|1591");
        CHECK(href(cs, "query:mutation-boundary-fairness-stats", "boundary-depth") >= 0, "depth");
        CHECK(href(cs, "query:mutation-boundary-fairness-stats", "per-fiber-stack-depth-max") >= 0,
              "per-fiber max");
        CHECK(href(cs, "query:mutation-boundary-fairness-stats",
                   "steal-inner-deferred-starvation-mitigated-count") >= 0,
              "steal mitigation");
        CHECK(href(cs, "query:mutation-boundary-fairness-stats",
                   "mutation-stack-depth-histogram-samples") >= 0,
              "histogram samples");
    }

    static void ac4_yield_vs_held(CompilerService& cs) {
        std::println("\n--- AC4: yield when free vs skip when held ---");
        auto& ev = cs.evaluator();
        const int rc0 = ev.try_safe_yield_at_boundary(0);
        CHECK(rc0 == 0, "safe yield at depth 0 returns 0");
        bool ok = true;
        {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            Evaluator::MutationBoundaryGuard guard(ev, &ok);
#pragma GCC diagnostic pop
            const int rc1 = ev.try_safe_yield_at_boundary(0);
            CHECK(rc1 == 1, "safe yield under Guard returns 1 (skipped-held)");
        }
        const int rc2 = ev.try_safe_yield_at_boundary(0);
        CHECK(rc2 == 0, "safe yield after Guard returns 0");
    }

    static void ac5_steal_metric_surface(CompilerService& cs) {
        std::println("\n--- AC5: steal starvation metric surface ---");
        auto& s = aura::serve::metrics::adaptive_steal_stats();
        const auto m0 = s.steal_inner_deferred_starvation_mitigated_count.load();
        s.steal_inner_deferred_starvation_mitigated_count.fetch_add(1, std::memory_order_relaxed);
        CHECK(href(cs, "query:mutation-boundary-fairness-stats",
                   "steal-inner-deferred-starvation-mitigated-count") >=
                  static_cast<std::int64_t>(m0 + 1),
              "fairness sees steal mitigation bump");
        CHECK(href(cs, "query:orchestration-steal-stats",
                   "steal-inner-deferred-starvation-mitigated-count") >=
                  static_cast<std::int64_t>(m0 + 1),
              "orchestration-steal-stats still exposes mitigation");
    }

    static void ac6_concurrent_probes(CompilerService& cs) {
        std::println("\n--- AC6: concurrent fairness probes ---");
        std::atomic<int> done{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&cs, &done] {
                for (int i = 0; i < 20; ++i) {
                    (void)cs.evaluator().try_safe_yield_at_boundary(0);
                    (void)cs.eval("(engine:metrics \"query:mutation-boundary-fairness-stats\")");
                }
                done.fetch_add(1, std::memory_order_relaxed);
            });
        }
        for (auto& th : threads)
            th.join();
        CHECK(done.load() == 4, "all probe threads finished");
    }

    static void run_1591_safe_yield_fairness() {
        std::println("\n=== Issue #1591: mutation-boundary safe-yield + fairness ===");
        CompilerService cs;
        ac1_safe_yield_schema(cs);
        ac2_depth_stats(cs);
        ac3_fairness_dashboard(cs);
        ac4_yield_vs_held(cs);
        ac5_steal_metric_surface(cs);
        ac6_concurrent_probes(cs);
    }

} // namespace _1591_detail

// ---------------------------------------------------------------------------
// Issue #1444: mutation-boundary full coverage audit (5 ACs)
// ---------------------------------------------------------------------------
namespace _1444_detail {

    static bool ac4_primitive_shape(CompilerService& cs) {
        auto r = cs.eval("(engine:metrics \"query:mutation-boundary-coverage-stats\")");
        if (!r || !is_hash(*r)) {
            TEST_LOG("AC4: primitive did not return hash");
            return false;
        }
        auto schema_ev = cs.eval(
            "(hash-get (engine:metrics \"query:mutation-boundary-coverage-stats\") 'schema)");
        if (!schema_ev || !is_int(*schema_ev) || as_int(*schema_ev) != 1444) {
            TEST_LOG("AC4: schema field != 1444");
            return false;
        }
        return true;
    }

    static bool ac2_fresh_service_zero(CompilerService& cs) {
        auto r = cs.eval("(hash-get (engine:metrics \"query:mutation-boundary-coverage-stats\") "
                         "'naked-mutate-attempt)");
        if (!r || !is_int(*r) || as_int(*r) != 0) {
            TEST_LOG("AC2: expected 0 on fresh service");
            return false;
        }
        return true;
    }

    static bool ac1_depth_zero_idle(CompilerService& cs) {
        auto r = cs.eval("(hash-get (engine:metrics \"query:mutation-boundary-coverage-stats\") "
                         "'boundary-depth)");
        if (!r || !is_int(*r) || as_int(*r) != 0) {
            TEST_LOG("AC1: expected depth=0 idle");
            return false;
        }
        return true;
    }

    static bool ac6_basic_mutate_cycle(CompilerService& cs) {
        if (!cs.eval("(set-code \"(define x 1) (set! x 42)\")")) {
            TEST_LOG("AC6: set-code failed");
            return false;
        }
        auto r = cs.eval("(eval-current)");
        if (!r || !is_int(*r) || as_int(*r) != 42) {
            TEST_LOG("AC6: mutate cycle broke — got non-42 result");
            return false;
        }
        auto d = cs.eval("(hash-get (engine:metrics \"query:mutation-boundary-coverage-stats\") "
                         "'boundary-depth)");
        if (!d || !is_int(*d) || as_int(*d) != 0) {
            TEST_LOG("AC6: depth did not return to 0 after cycle");
            return false;
        }
        return true;
    }

    static bool ac1_no_naked_after_mutate_cycle(CompilerService& cs) {
        const std::int64_t before = [&]() -> std::int64_t {
            auto r =
                cs.eval("(hash-get (engine:metrics \"query:mutation-boundary-coverage-stats\") "
                        "'naked-mutate-attempt)");
            return (r && is_int(*r)) ? as_int(*r) : 0;
        }();
        if (!cs.eval("(set-code \"(define xs '(1 2 3)) (set-car! xs 99) (car xs)\")")) {
            TEST_LOG("AC1: set-car! setup failed");
            return false;
        }
        if (auto r = cs.eval("(eval-current)"); !r || !is_int(*r) || as_int(*r) != 99) {
            TEST_LOG("AC1: set-car! returned non-99");
            return false;
        }
        const std::int64_t after = [&]() -> std::int64_t {
            auto r =
                cs.eval("(hash-get (engine:metrics \"query:mutation-boundary-coverage-stats\") "
                        "'naked-mutate-attempt)");
            return (r && is_int(*r)) ? as_int(*r) : 0;
        }();
        if (after != before) {
            TEST_LOG("AC1: naked-mutate-attempt bumped during set-car! cycle");
            return false;
        }
        return true;
    }

    static void run_1444_full_coverage() {
        std::println("\n=== Issue #1444: mutation-boundary full coverage audit ===");
        CompilerService cs;
        CHECK(ac4_primitive_shape(cs), "AC4 primitive shape");
        CHECK(ac2_fresh_service_zero(cs), "AC2 fresh service zero");
        CHECK(ac1_depth_zero_idle(cs), "AC1 depth zero idle");
        CHECK(ac6_basic_mutate_cycle(cs), "AC6 basic mutate cycle");
        CHECK(ac1_no_naked_after_mutate_cycle(cs), "AC1 no naked after mutate cycle");
    }

} // namespace _1444_detail

// ---------------------------------------------------------------------------
// Issue #417: mutation-boundary invariant closed loop (7 ACs)
// ---------------------------------------------------------------------------
namespace _417_detail {

    static std::int64_t boundary_invariant_stats(CompilerService& cs) {
        auto r = cs.eval("(engine:metrics \"query:mutation-boundary-invariant-stats\")");
        if (!r || !is_int(*r))
            return 0;
        return as_int(*r);
    }

    static bool setup_workspace(CompilerService& cs) {
        if (!cs.eval("(set-code \""
                     "(define (add1 x) (+ x 1)) "
                     "(define base 10) (define acc 0) "
                     "(add1 1)\")")) {
            return false;
        }
        return cs.eval("(eval-current)").has_value();
    }

    static void run_417_invariant_closed_loop() {
        std::println("\n=== Issue #417: mutation-boundary invariant closed loop ===");
        CompilerService cs;

        std::println("\n--- AC1: query:mutation-boundary-invariant-stats ---");
        CHECK(setup_workspace(cs), "mutation boundary workspace setup");
        const auto s0 = boundary_invariant_stats(cs);
        std::println("  mutation-boundary-invariant-stats = {}", s0);
        CHECK(s0 >= 0, "boundary invariant stats non-negative");

        std::println("\n--- AC2: mutate:rebind bumps epoch counters ---");
        const auto stats2a = boundary_invariant_stats(cs);
        (void)cs.eval("(mutate:rebind \"base\" \"99\")");
        const auto stats2b = boundary_invariant_stats(cs);
        std::println("  boundary invariant stats: {} -> {}", stats2a, stats2b);
        CHECK(stats2b > stats2a, "mutate bumps boundary invariant stats");

        std::println("\n--- AC3: nested enter/exit_mutation_boundary ---");
        auto& ev = cs.evaluator();
        ev.enter_mutation_boundary();
        ev.enter_mutation_boundary();
        ev.exit_mutation_boundary(true);
        ev.exit_mutation_boundary(true);
        CHECK(ev.get_total_invariant_violations() == 0,
              "nested boundary exit: zero invariant violations");

        std::println("\n--- AC4: ensure_mutation_invariants happy path ---");
        ev.ensure_mutation_invariants();
        CHECK(ev.get_total_invariant_violations() == 0,
              "explicit probe: zero invariant violations");

        std::println("\n--- AC5: eval-current materialize_call_env probe ---");
        const auto stats5a = boundary_invariant_stats(cs);
        CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
        const auto stats5b = boundary_invariant_stats(cs);
        std::println("  boundary invariant stats: {} -> {}", stats5a, stats5b);
        CHECK(stats5b >= stats5a, "eval monotonic for boundary invariant stats");

        std::println("\n--- AC6: multi-round mutate matrix ---");
        const auto stats6a = boundary_invariant_stats(cs);
        for (int round = 0; round < 3; ++round) {
            (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(round) + "\")");
            (void)cs.eval("(eval-current)");
        }
        const auto stats6b = boundary_invariant_stats(cs);
        std::println("  boundary invariant stats: {} -> {}", stats6a, stats6b);
        CHECK(stats6b > stats6a, "boundary invariant stats grow over matrix");
        CHECK(ev.get_total_invariant_violations() == 0, "matrix end: zero invariant violations");

        std::println("\n--- AC7: query regression ---");
        auto mcs = cs.eval("(engine:metrics \"query:mutation-coordination-stats\")");
        auto fms = cs.eval("(engine:metrics \"query:fiber-migration-stats\")");
        CHECK(mcs && is_int(*mcs), "mutation-coordination-stats regression");
        CHECK(fms && is_int(*fms), "fiber-migration-stats regression");
    }

} // namespace _417_detail

// ---------------------------------------------------------------------------
// Issue #548: panic-checkpoint rollback + fiber resume (11 ACs)
// ---------------------------------------------------------------------------
namespace _548_detail {

    static void run_548_panic_rollback_fiber() {
        std::println("\n=== Issue #548: panic-checkpoint rollback + fiber resume ===");

        std::println("\n--- AC1: panic-checkpoint lifecycle counters ---");
        {
            CompilerService cs;
            (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
            (void)cs.eval("(eval-current)");
            const auto s0 = cs.evaluator().get_panic_checkpoint_save_count();
            const auto r0 = cs.evaluator().get_panic_checkpoint_restore_count();
            const auto c0 = cs.evaluator().get_panic_checkpoint_commit_count();
            const auto rs0 = cs.evaluator().get_rollback_success_on_panic();
            auto r = cs.eval("(panic-checkpoint)");
            CHECK(r.has_value() && is_bool(*r) && as_bool(*r), "(panic-checkpoint) succeeded");
            const auto s1 = cs.evaluator().get_panic_checkpoint_save_count();
            CHECK(s1 > s0, "panic_checkpoint_save_count bumped");
            auto rr = cs.eval("(panic-restore)");
            CHECK(rr.has_value() && is_bool(*rr), "(panic-restore) succeeded");
            const auto r1 = cs.evaluator().get_panic_checkpoint_restore_count();
            const auto rs1 = cs.evaluator().get_rollback_success_on_panic();
            CHECK(r1 > r0, "panic_checkpoint_restore_count bumped");
            CHECK(rs1 > rs0, "rollback_success_on_panic bumped");
            (void)c0;
        }

        std::println("\n--- AC2: (query:panic-checkpoint-lifecycle-stats) ---");
        {
            CompilerService cs;
            (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
            (void)cs.eval("(eval-current)");
            for (int i = 0; i < 3; ++i) {
                (void)cs.eval("(panic-checkpoint)");
                (void)cs.eval("(mutate:replace-value (define a " + std::to_string(i) +
                              ") (define a " + std::to_string(i) + "))");
            }
            auto r = cs.eval("(engine:metrics \"query:panic-checkpoint-lifecycle-stats\")");
            CHECK(r.has_value() && is_int(*r), "primitive returns integer");
            if (r && is_int(*r)) {
                const auto v = as_int(*r);
                std::println("  query:panic-checkpoint-lifecycle-stats = {}", v);
                CHECK(v > 0, "stats > 0 after saves + mutates");
            }
        }

        std::println("\n--- AC3: Nested Guard basic ---");
        {
            Evaluator ev;
            const auto v0 = ev.defuse_version_for_test();
            {
                bool outer_ok = true;
                Evaluator::MutationBoundaryGuard outer(ev, &outer_ok);
                {
                    bool inner_ok = true;
                    Evaluator::MutationBoundaryGuard inner(ev, &inner_ok);
                    const auto depth = aura_evaluator_mutation_boundary_depth();
                    CHECK(depth >= 1, "depth >= 1 inside nested Guard");
                    (void)inner_ok;
                }
                const auto depth_after_inner = aura_evaluator_mutation_boundary_depth();
                CHECK(depth_after_inner >= 1, "depth >= 1 with outer Guard");
            }
            const auto depth_final = aura_evaluator_mutation_boundary_depth();
            CHECK(depth_final == 0, "depth == 0 after all Guards exit");
            const auto v1 = ev.defuse_version_for_test();
            CHECK(v1 > v0, "defuse_version_ bumped after nested Guard");
        }

        std::println("\n--- AC4: Panic at depth 0 (defensive) ---");
        {
            CompilerService cs;
            (void)cs.eval("(set-code \"(define a 1)\")");
            (void)cs.eval("(eval-current)");
            const auto depth0 = aura_evaluator_mutation_boundary_depth();
            CHECK(depth0 == 0, "depth == 0 with no Guard active");
            auto r = cs.eval("(panic-checkpoint)");
            CHECK(r.has_value() && is_bool(*r), "(panic-checkpoint) at depth 0 returns");
            auto rr = cs.eval("(panic-restore)");
            CHECK(rr.has_value(), "(panic-restore) at depth 0 returns");
        }

        std::println("\n--- AC5: panic-restore lifecycle ---");
        {
            CompilerService cs;
            (void)cs.eval("(set-code \"(define x 1)\")");
            (void)cs.eval("(eval-current)");
            const auto s0 = cs.evaluator().get_panic_checkpoint_save_count();
            const auto r0 = cs.evaluator().get_panic_checkpoint_restore_count();
            const auto rs0 = cs.evaluator().get_rollback_success_on_panic();
            (void)cs.eval("(panic-checkpoint)");
            (void)cs.eval("(mutate:replace-value (define x 99) (define x 99))");
            auto rr = cs.eval("(panic-restore)");
            CHECK(rr.has_value() && is_bool(*rr), "(panic-restore) returned");
            const auto s1 = cs.evaluator().get_panic_checkpoint_save_count();
            const auto r1 = cs.evaluator().get_panic_checkpoint_restore_count();
            const auto rs1 = cs.evaluator().get_rollback_success_on_panic();
            CHECK(s1 > s0, "save_count bumped");
            CHECK(r1 > r0, "restore_count bumped");
            CHECK(rs1 > rs0, "rollback_success bumped on successful restore");
        }

        std::println("\n--- AC6: panic-commit lifecycle ---");
        {
            Evaluator ev;
            const auto s0 = ev.get_panic_checkpoint_save_count();
            const auto c0 = ev.get_panic_checkpoint_commit_count();
            const auto r0 = ev.get_panic_checkpoint_restore_count();
            ev.set_panic_safe_cells_size_for_test(10);
            ev.bump_panic_checkpoint_save_count();
            ev.bump_panic_checkpoint_save_count();
            ev.commit_panic_checkpoint();
            const auto s1 = ev.get_panic_checkpoint_save_count();
            const auto c1 = ev.get_panic_checkpoint_commit_count();
            const auto r1 = ev.get_panic_checkpoint_restore_count();
            CHECK(s1 == s0 + 2, "save_count bumped by 2");
            CHECK(c1 == c0 + 1, "commit_count bumped by 1");
            CHECK(r1 == r0, "restore_count unchanged");
        }

        std::println("\n--- AC7: per-fiber depth probe ---");
        {
            constexpr int k_threads = 4;
            std::atomic<int> depth_nonzero_count{0};
            std::vector<std::thread> threads;
            for (int t = 0; t < k_threads; ++t) {
                threads.emplace_back([&depth_nonzero_count, t]() {
                    Evaluator ev;
                    {
                        bool ok = true;
                        Evaluator::MutationBoundaryGuard guard(ev, &ok);
                        const auto d = aura_evaluator_mutation_boundary_depth();
                        if (d > 0)
                            depth_nonzero_count.fetch_add(1);
                    }
                    const auto d_after = aura_evaluator_mutation_boundary_depth();
                    (void)d_after;
                });
            }
            for (auto& th : threads)
                th.join();
            CHECK(depth_nonzero_count.load() == k_threads,
                  "all 4 threads observed depth > 0 (per-thread isolation)");
        }

        std::println("\n--- AC8: nested mutate + random panic fuzz ---");
        {
            const int kIters = k_fuzz_iters();
            CompilerService cs;
            (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
            (void)cs.eval("(eval-current)");
            std::mt19937 rng(548u);
            std::uniform_int_distribution<int> val_dist(0, 999);
            std::uniform_int_distribution<int> panic_every(11, 31);
            int panics = 0;
            int next_panic = panic_every(rng);
            const auto s0 = cs.evaluator().get_panic_checkpoint_save_count();
            const auto r0 = cs.evaluator().get_panic_checkpoint_restore_count();
            for (int i = 0; i < kIters; ++i) {
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
            CHECK(panics > 0, "at least 1 panic checkpoint + restore executed");
            CHECK(s1 >= s0 + static_cast<std::uint64_t>(panics), "save_count bumped");
            CHECK(r1 >= r0 + static_cast<std::uint64_t>(panics), "restore_count bumped");
        }

        std::println("\n--- AC9: (gc-heap) under panic-checkpoint ---");
        {
            CompilerService cs;
            (void)cs.eval("(set-code \"(define x 1) (define y 2)\")");
            (void)cs.eval("(eval-current)");
            auto r1 = cs.eval("(panic-checkpoint)");
            CHECK(r1.has_value(), "(panic-checkpoint) succeeded");
            auto r2 = cs.eval("(gc-heap)");
            CHECK(r2.has_value(), "(gc-heap) callable under panic-checkpoint");
            auto r3 = cs.eval("(panic-restore)");
            CHECK(r3.has_value(), "(panic-restore) succeeded after (gc-heap)");
        }

        std::println("\n--- AC10: 8-thread concurrent nested mutate ---");
        {
            CompilerService cs;
            (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
            (void)cs.eval("(eval-current)");
            constexpr int n_threads = 8;
            constexpr int n_iters = 20;
            std::mutex mtx;
            std::atomic<int> completed{0};
            auto worker = [&](int tid) {
                for (int i = 0; i < n_iters; ++i) {
                    std::lock_guard<std::mutex> lk(mtx);
                    std::string code =
                        "(define v" + std::to_string(tid) + " " + std::to_string(i) + ")";
                    (void)cs.eval(code);
                    completed.fetch_add(1);
                }
            };
            std::vector<std::thread> threads;
            for (int i = 0; i < n_threads; ++i)
                threads.emplace_back(worker, i);
            for (auto& t : threads)
                t.join();
            CHECK(completed.load() == n_threads * n_iters, "all 160 ops completed");
            const auto s = cs.evaluator().get_panic_checkpoint_save_count();
            const auto r = cs.evaluator().get_panic_checkpoint_restore_count();
            const auto c = cs.evaluator().get_panic_checkpoint_commit_count();
            CHECK(s >= 0 && r >= 0 && c >= 0, "lifecycle counters non-negative");
        }

        std::println("\n--- AC11: regression — existing primitives still work ---");
        {
            CompilerService cs;
            auto r1 = cs.eval("(panic-checkpoint)");
            CHECK(r1.has_value(), "(panic-checkpoint) regression");
            auto r2 = cs.eval("(engine:metrics \"query:panic-checkpoint-lifecycle-stats\")");
            CHECK(r2.has_value() && is_int(*r2), "query:panic-checkpoint-lifecycle-stats");
            auto r3 = cs.eval("(engine:metrics \"query:envframe-dualpath-stats\")");
            CHECK(r3.has_value() && is_int(*r3), "query:envframe-dualpath-stats regression (#543)");
            auto r4 = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
            CHECK(r4.has_value() && is_int(*r4), "query:pattern-hygiene-stats regression (#547)");
            if (!cs.eval("(define reg-548-a 10)")) {
                CHECK(false, "define regression");
                return;
            }
            (void)cs.eval("(define reg-548-b 32)");
            auto r6 = cs.eval("(+ reg-548-a reg-548-b)");
            CHECK(r6.has_value() && is_int(*r6) && as_int(*r6) == 42,
                  "(+ reg-548-a reg-548-b) == 42");
        }
    }

} // namespace _548_detail

} // namespace aura_mutation_boundary_batch

int main() {
    std::println("=== B pilot #10: mutation_boundary family batch "
                 "(#1591 + #1444 + #417 + #548) ===");
    aura_mutation_boundary_batch::_1591_detail::run_1591_safe_yield_fairness();
    aura_mutation_boundary_batch::_1444_detail::run_1444_full_coverage();
    aura_mutation_boundary_batch::_417_detail::run_417_invariant_closed_loop();
    aura_mutation_boundary_batch::_548_detail::run_548_panic_rollback_fiber();
    return RUN_ALL_TESTS();
}
