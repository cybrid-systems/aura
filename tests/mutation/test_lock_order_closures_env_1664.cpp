// @category: unit
// @reason: Issue #1664 — lock order closures_mtx_ → env_frames_mtx_
// must be consistent across probe_linear_ownership_* and
// scan_live_closures_for_linear_captures (no reverse-order deadlock).
//
//   AC1: scan_live_closures alone completes
//   AC2: probe at GC safepoint alone completes
//   AC3: probe on fiber steal alone completes
//   AC4: concurrent scan + GC probe + fiber steal (no hang)
//   AC5: concurrent with apply_closure (no hang)
//   AC6: stress 4 threads × 500 ops completes quickly

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::SymId;
using aura::compiler::Closure;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

static void seed_moved_closure(Evaluator& ev) {
    auto env_id = ev.alloc_env_frame(NULL_ENV_ID);
    if (auto* fr = ev.resolve_env_frame_mut(env_id)) {
        fr->bindings_symid_.push_back({static_cast<SymId>(1), make_int(0)});
        fr->bindings_linear_ownership_state_.push_back(4); // Moved
        fr->version_ = ev.defuse_version_snapshot();
    }
    Closure cl;
    cl.env_id = env_id;
    (void)ev.register_active_closure(std::move(cl));
}

static void ac1_scan() {
    std::println("\n--- AC1: scan_live_closures ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    seed_moved_closure(ev);
    auto r = ev.scan_live_closures_for_linear_captures(true, true);
    CHECK(r.examined >= 1, "examined ≥1");
}

static void ac2_gc_probe() {
    std::println("\n--- AC2: probe at GC safepoint ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    seed_moved_closure(ev);
    ev.test_probe_linear_at_gc_safepoint();
    CHECK(true, "gc safepoint probe completed");
}

static void ac3_steal_probe() {
    std::println("\n--- AC3: probe on fiber steal ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    seed_moved_closure(ev);
    ev.test_probe_linear_on_fiber_steal();
    CHECK(true, "fiber steal probe completed");
}

static void ac4_concurrent_probes() {
    std::println("\n--- AC4: concurrent scan + probes ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 8; ++i)
        seed_moved_closure(ev);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> ops{0};
    std::vector<std::thread> thr;

    thr.emplace_back([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)ev.scan_live_closures_for_linear_captures(true, false);
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    });
    thr.emplace_back([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            ev.test_probe_linear_at_gc_safepoint();
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    });
    thr.emplace_back([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            ev.test_probe_linear_on_fiber_steal();
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const auto t0 = std::chrono::steady_clock::now();
    while (ops.load(std::memory_order_relaxed) < 150 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : thr)
        t.join();

    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(ops.load() >= 150, std::format("ops ≥150 (got {})", ops.load()));
    CHECK(elapsed < std::chrono::seconds(5), "completed under 5s (no deadlock hang)");
}

static void ac5_with_apply() {
    std::println("\n--- AC5: concurrent with apply_closure ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    seed_moved_closure(ev);
    CHECK(cs.eval("(set-code \"(define (f x) x)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> ops{0};
    std::thread probe([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            ev.test_probe_linear_at_gc_safepoint();
            (void)ev.scan_live_closures_for_linear_captures(false, false);
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread apply([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)cs.eval("(f 1)");
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const auto t0 = std::chrono::steady_clock::now();
    while (ops.load(std::memory_order_relaxed) < 100 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true, std::memory_order_relaxed);
    probe.join();
    apply.join();
    CHECK(ops.load() >= 100, "apply+probe ops ≥100");
    CHECK(true, "no hang with apply_closure");
}

static void ac6_stress() {
    std::println("\n--- AC6: 4-thread stress ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 4; ++i)
        seed_moved_closure(ev);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> total{0};
    auto worker = [&](int kind) {
        while (!stop.load(std::memory_order_relaxed)) {
            switch (kind % 3) {
                case 0:
                    (void)ev.scan_live_closures_for_linear_captures(true, true);
                    break;
                case 1:
                    ev.test_probe_linear_at_gc_safepoint();
                    break;
                default:
                    ev.test_probe_linear_on_fiber_steal();
                    break;
            }
            total.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> thr;
    for (int i = 0; i < 4; ++i)
        thr.emplace_back(worker, i);

    const auto t0 = std::chrono::steady_clock::now();
    while (total.load(std::memory_order_relaxed) < 500 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : thr)
        t.join();
    CHECK(total.load() >= 500, std::format("stress ≥500 (got {})", total.load()));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8), "stress under 8s");
}

} // namespace

int main() {
    std::println("=== Issue #1664: closures→env lock order (no reverse deadlock) ===");
    ac1_scan();
    ac2_gc_probe();
    ac3_steal_probe();
    ac4_concurrent_probes();
    ac5_with_apply();
    ac6_stress();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
