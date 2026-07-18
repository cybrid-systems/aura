// @category: unit
// @reason: Issue #1663 — set_arena owner pair atomic w.r.t. concurrent
// allocate_raw (no torn owner/fn → silent quota bypass)
// (refine #1546 / #1554 / #1662).
//
//   AC1: has_arena_owner is all-or-nothing under concurrent set/clear
//   AC2: concurrent try_allocate + set_arena_owner/clear does not crash
//   AC3: concurrent set_arena flip + allocate under Evaluator
//   AC4: quota still enforced after set_arena (not bypassed)
//   AC5: #1662 dtor clear still holds under concurrent allocate
//   AC6: stress 4 threads × 2000 ops; no crash; owner consistent

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.core.arena;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace {

using aura::ast::ASTArena;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

// AC1: direct arena owner set/clear never leaves half-state for readers.
static void ac1_owner_pair_atomic() {
    std::println("\n--- AC1: owner pair all-or-nothing ---");
    ASTArena arena(256 * 1024);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> inconsistent{0};
    std::atomic<std::uint64_t> checks{0};

    std::thread reader([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            // Single-lock consistency: owner and allow_fn both set or both null.
            // (Separate has_arena_owner() + arena_owner() can race; not a tear.)
            if (!arena.owner_pair_consistent())
                inconsistent.fetch_add(1, std::memory_order_relaxed);
            checks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    Evaluator ev;
    for (int i = 0; i < 500; ++i) {
        arena.set_arena_owner(&ev, &Evaluator::arena_quota_allow);
        arena.clear_arena_owner();
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    CHECK(checks.load() > 0, "reader performed checks");
    CHECK(inconsistent.load() == 0,
          std::format("no torn has/owner (inconsistent={})", inconsistent.load()));
}

static void ac2_concurrent_allocate_set_owner() {
    std::println("\n--- AC2: concurrent try_allocate + set/clear owner ---");
    ASTArena arena(512 * 1024);
    Evaluator ev;
    ev.set_resource_quota_memory(1024);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> allocs{0};
    std::atomic<std::uint64_t> rejects{0};

    std::thread worker([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            void* p = arena.try_allocate(64);
            if (p)
                allocs.fetch_add(1, std::memory_order_relaxed);
            else
                rejects.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < 1000; ++i) {
        arena.set_arena_owner(&ev, &Evaluator::arena_quota_allow);
        arena.clear_arena_owner();
    }
    stop.store(true, std::memory_order_relaxed);
    worker.join();

    CHECK(allocs.load() + rejects.load() > 0, "worker made progress");
    CHECK(true, "no crash under concurrent allocate + set/clear");
}

static void ac3_set_arena_flip() {
    std::println("\n--- AC3: concurrent set_arena flip + allocate ---");
    ASTArena a(256 * 1024);
    ASTArena b(256 * 1024);
    Evaluator ev;
    ev.set_resource_quota_memory(4096);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> ops{0};

    std::thread alloc_a([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)a.try_allocate(32);
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread alloc_b([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)b.try_allocate(32);
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < 400; ++i) {
        if ((i % 2) == 0)
            ev.set_arena(&a);
        else
            ev.set_arena(&b);
    }
    stop.store(true, std::memory_order_relaxed);
    alloc_a.join();
    alloc_b.join();

    CHECK(ops.load() > 0, "allocators made progress");
    // Final arena should have a consistent owner.
    if (a.has_arena_owner())
        CHECK(a.arena_owner() == static_cast<void*>(&ev), "a owner is ev if set");
    if (b.has_arena_owner())
        CHECK(b.arena_owner() == static_cast<void*>(&ev), "b owner is ev if set");
    CHECK(true, "set_arena flip stress completed");
}

static void ac4_quota_enforced() {
    std::println("\n--- AC4: quota still enforced after set_arena ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(64);

    const auto rejects0 = m->resource_quota_rejects_total.load(std::memory_order_relaxed);
    void* p = arena.try_allocate(4096);
    CHECK(p == nullptr, "over-quota try_allocate → nullptr");
    CHECK(m->resource_quota_rejects_total.load(std::memory_order_relaxed) > rejects0,
          "rejects advanced (quota not bypassed)");
}

static void ac5_dtor_under_allocate() {
    std::println("\n--- AC5: dtor clear under concurrent allocate ---");
    auto arena = std::make_shared<ASTArena>(256 * 1024);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> ops{0};

    std::thread worker([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)arena->try_allocate(16);
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    });

    {
        Evaluator ev;
        ev.set_arena(arena.get());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } // dtor clears owner while worker allocates

    stop.store(true, std::memory_order_relaxed);
    worker.join();
    CHECK(!arena->has_arena_owner(), "owner cleared after dtor");
    CHECK(ops.load() > 0, "worker progressed");
    void* p = arena->try_allocate(64);
    CHECK(p != nullptr, "post-dtor allocate ok");
}

static void ac6_stress() {
    std::println("\n--- AC6: 4-thread stress ---");
    ASTArena arena(1024 * 1024);
    Evaluator ev;
    ev.set_resource_quota_memory(2048);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> total{0};
    std::atomic<std::uint64_t> torn{0};

    auto worker = [&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)arena.try_allocate(48);
            if (!arena.owner_pair_consistent())
                torn.fetch_add(1, std::memory_order_relaxed);
            total.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> thr;
    for (int i = 0; i < 3; ++i)
        thr.emplace_back(worker);

    for (int i = 0; i < 2000; ++i) {
        if ((i % 2) == 0)
            arena.set_arena_owner(&ev, &Evaluator::arena_quota_allow);
        else
            arena.clear_arena_owner();
        total.fetch_add(1, std::memory_order_relaxed);
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : thr)
        t.join();

    CHECK(total.load() > 2000, "stress ops completed");
    CHECK(torn.load() == 0, std::format("no torn observations (torn={})", torn.load()));
}

} // namespace

int main() {
    std::println("=== Issue #1663: set_arena atomic owner vs allocate_raw ===");
    ac1_owner_pair_atomic();
    ac2_concurrent_allocate_set_owner();
    ac3_set_arena_flip();
    ac4_quota_enforced();
    ac5_dtor_under_allocate();
    ac6_stress();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
