// @category: integration
// @reason: Issue #1544 — stress test linear post-mutate under AI self-modify
// + GC safepoint + concurrent fiber steal (parent #1478 AC #5).
//
//   AC1: 10K+ iter loop: mutation + GC safepoint + fiber steal per iter
//   AC2: linear_post_mutate_enforcements monotonic
//   AC3: Moved inject → violation_prevented matches expected count
//   AC4: concurrent steal workers while main mutates (no crash)
//   AC5: wall-clock budget ≤ 60s (CI gate)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1544_detail {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

constexpr std::uint8_t kOwned = 1;
constexpr std::uint8_t kMoved = 4;

// Default 10K; override with AURA_STRESS_ITERS for nightlies / debug.
static int stress_iters() {
    if (const char* e = std::getenv("AURA_STRESS_ITERS")) {
        int v = std::atoi(e);
        if (v >= 1000)
            return v;
    }
    return 10'000;
}

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

// AC1 + AC2 + AC5: 10K mutate → safepoint → steal; enforcements monotonic; ≤60s.
static void ac1_10k_stress_loop() {
    std::println("\n--- AC1/AC2/AC5: 10K stress (mutate + safepoint + steal) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics available");

    CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1)))\")").has_value(), "set-code f");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current f");

    // Stable Owned frame scanned every iter (enforce path warm).
    aura::compiler::Env owned_src;
    owned_src.bind_symid_with_linear_state(1, make_int(1), kOwned);
    const auto owned_id = ev.alloc_env_frame_from_env(owned_src);
    CHECK(owned_id != NULL_ENV_ID, "owned frame allocated");

    const int kIters = stress_iters();
    const auto enf0 = load_u64(m->linear_post_mutate_enforcements);
    const auto pipe0 = load_u64(m->linear_post_mutate_pipeline_total);
    std::uint64_t prev_enf = enf0;
    int mono_ok = 0;
    int owned_ok = 0;
    int typed_samples = 0;

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
        // --- AI mutation ---
        // Dual-epoch write every iter (#1476 self-modify write-side).
        cs.public_atomic_bump_epochs_and_stamp_bridge("f");
        // Full typed_mutate pipeline every 50th iter (CI budget).
        if ((i % 50) == 0) {
            auto mr = cs.public_typed_mutate(std::format(
                "(mutate:rebind \"f\" \"(lambda (x) (+ x {}))\" \"#1544\")", (i % 7) + 1));
            if (mr.success)
                ++typed_samples;
        }

        // --- GC pressure ---
        (void)ev.request_gc_safepoint();

        // --- Fiber steal ---
        ev.probe_linear_ownership_on_fiber_steal();

        // --- Linear enforce (Owned) — quiet (no per-iter CHECK spam) ---
        if (ev.linear_post_mutate_enforce(owned_id))
            ++owned_ok;
        const auto enf = load_u64(m->linear_post_mutate_enforcements);
        if (enf >= prev_enf)
            ++mono_ok;
        prev_enf = enf;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    CHECK(owned_ok == kIters, "Owned frame safe every iter");
    CHECK(mono_ok == kIters, "enforcements monotonic on every iter");
    CHECK(load_u64(m->linear_post_mutate_enforcements) >= enf0 + static_cast<std::uint64_t>(kIters),
          "enforcements grew by ≥ kIters (Owned scan each iter)");
    CHECK(typed_samples > 0, "at least one typed_mutate sample succeeded");
    CHECK(load_u64(m->linear_post_mutate_pipeline_total) >= pipe0 + 1, "pipeline advanced");
    // AC5: CI budget 60s (quiet loop; dual-epoch + periodic typed_mutate).
    CHECK(ms <= 60'000, std::format("wall-clock {}ms ≤ 60000ms", ms));
    std::println("  iters={} typed_samples={} enforcements {}→{} wall={}ms", kIters, typed_samples,
                 enf0, load_u64(m->linear_post_mutate_enforcements), ms);
}

// AC3: under pressure, Moved still detected; violation_prevented delta matches injects.
static void ac3_moved_under_pressure() {
    std::println("\n--- AC3: Moved inject under stress → violation_prevented ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    CHECK(cs.eval("(set-code \"(define g (lambda () 0))\")").has_value(), "set-code g");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current g");

    constexpr int kRounds = 500;
    constexpr int kInjectEvery = 50; // 10 Moved injects
    constexpr int kExpectedInjects = kRounds / kInjectEvery;

    const auto viol0 = load_u64(m->linear_ownership_violation_prevented);
    const auto enf0 = load_u64(m->linear_post_mutate_enforcements);
    int injects = 0;
    int moved_caught = 0;

    for (int i = 0; i < kRounds; ++i) {
        cs.public_atomic_bump_epochs_and_stamp_bridge("g");
        if ((i % 10) == 0) {
            (void)cs.public_typed_mutate(
                std::format("(mutate:rebind \"g\" \"(lambda () {})\" \"#1544-moved\")", i % 3));
        }
        (void)ev.request_gc_safepoint();
        ev.probe_linear_ownership_on_fiber_steal();

        if ((i % kInjectEvery) == 0) {
            aura::compiler::Env src;
            src.bind_symid_with_linear_state(200 + injects, make_int(injects), kMoved);
            auto mid = ev.alloc_env_frame_from_env(src);
            ++injects;
            if (!ev.linear_post_mutate_enforce(mid))
                ++moved_caught;
        } else {
            // Safe Owned enforce keeps counter warm.
            aura::compiler::Env src;
            src.bind_symid_with_linear_state(9, make_int(i), kOwned);
            auto oid = ev.alloc_env_frame_from_env(src);
            (void)ev.linear_post_mutate_enforce(oid);
        }
    }

    const auto viol1 = load_u64(m->linear_ownership_violation_prevented);
    const auto enf1 = load_u64(m->linear_post_mutate_enforcements);

    CHECK(injects == kExpectedInjects, "inject count");
    CHECK(moved_caught == injects, "every Moved inject failed enforce");
    // linear_post_mutate_enforce bumps violation_prevented once per Moved frame.
    CHECK(viol1 >= viol0 + static_cast<std::uint64_t>(injects), "violation_prevented ≥ injects");
    CHECK(enf1 >= enf0 + static_cast<std::uint64_t>(kRounds),
          "enforcements grew across all rounds");
    std::println("  injects={} caught={} viol {}→{} enf {}→{}", injects, moved_caught, viol0, viol1,
                 enf0, enf1);
}

// AC4: concurrent fiber-steal probes while main does mutate/safepoint/enforce.
static void ac4_concurrent_steal_workers() {
    std::println("\n--- AC4: concurrent steal workers + main mutate ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    CHECK(cs.eval("(set-code \"(define h (lambda (x) x))\")").has_value(), "set-code h");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current h");

    // Live closure so steal probe has something to walk.
    (void)cs.eval("(lambda (z) (h z))");

    aura::compiler::Env owned_src;
    owned_src.bind_symid_with_linear_state(3, make_int(3), kOwned);
    const auto owned_id = ev.alloc_env_frame_from_env(owned_src);

    constexpr int kWorkers = 4;
    constexpr int kMainIters = 1000;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> steal_calls{0};
    std::atomic<int> errors{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&]() {
            try {
                while (!stop.load(std::memory_order_relaxed)) {
                    ev.probe_linear_ownership_on_fiber_steal();
                    steal_calls.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                errors.fetch_add(1);
            }
        });
    }

    const auto enf0 = load_u64(m->linear_post_mutate_enforcements);
    int owned_ok = 0;
    for (int i = 0; i < kMainIters; ++i) {
        cs.public_atomic_bump_epochs_and_stamp_bridge("h");
        if ((i % 50) == 0) {
            (void)cs.public_typed_mutate(std::format(
                "(mutate:rebind \"h\" \"(lambda (x) (+ x {}))\" \"#1544-conc\")", (i % 5) + 1));
        }
        (void)ev.request_gc_safepoint();
        if (ev.linear_post_mutate_enforce(owned_id))
            ++owned_ok;
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : workers)
        t.join();

    CHECK(errors.load() == 0, "no exceptions in steal workers");
    CHECK(steal_calls.load() > 0, "steal workers ran");
    CHECK(owned_ok == kMainIters, "Owned safe under concurrent steal (all iters)");
    CHECK(load_u64(m->linear_post_mutate_enforcements) >=
              enf0 + static_cast<std::uint64_t>(kMainIters),
          "main enforcements advanced");
    std::println("  main_iters={} steal_calls={} enforcements {}→{}", kMainIters,
                 steal_calls.load(), enf0, load_u64(m->linear_post_mutate_enforcements));
}

} // namespace aura_issue_1544_detail

int main() {
    using namespace aura_issue_1544_detail;
    std::println("=== Issue #1544: linear post-mutate stress (AI + GC + fiber) ===");
    ac1_10k_stress_loop();
    ac3_moved_under_pressure();
    ac4_concurrent_steal_workers();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
