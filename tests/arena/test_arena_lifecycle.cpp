// test_arena_lifecycle.cpp — Merged #1947/#1954 + #300 + #1359 (Anqi 2026-07-21).
//
// Originally 3 files in tests/arena/:
//   - test_closure_bridge_lifetime_safety.cpp (Issue #1947/#1954, P0 ClosureView UAF)
//   - test_pcv_heap_use_after_free.cpp (Issue #300 follow-up #1, PCV heap UAF regression)
//   - test_tl_arena_capacity.cpp (Issue #1359, TLarena 1MB default + graceful OOM)
//
// All three cover arena memory lifetime/safety/capacity. Merged with all ACs
// preserved verbatim. The ClosureView UAF + PCV UAF pair share the UAF theme;
// TLarena capacity tests the C TLarena struct directly (no CompilerService).
//
// AC list (all preserved; each section cites original issue#):
//   Issue #1947/#1954 (closure_bridge_lifetime_safety.cpp):
//     AC1: make_closure_view stamps source_lifetime_version + live on a fresh
//          Closure; soft + strong validity checks pass
//     AC2: invalidate_closure_lifetime tombstone → strong check fails,
//          g_closure_view_invalid_access_total++
//     AC3: make_closure_view on already-tombstoned Closure returns invalid view;
//          g_closure_view_dangling_prevented_total++
//     AC4: safe accessors (closure_view_flat/pool/owner_arena) on invalid view
//          return nullptr; dangling_prevented_total++
//     AC5: g_closure_view_invalid_access_total increments only on
//          strong-revalidation failure (lifetime_version mismatch), not on
//          soft-rejection at view-creation time
//     AC6: long-running concurrent self-evolve + GC pressure loop:
//          N threads make views, N/2 threads invalidate closures,
//          N/2 threads read views via safe accessors — no UAF
//   Issue #300 follow-up #1 (pcv_heap_use_after_free.cpp):
//     AC1: Regression — set-code + (arena:defrag) + (stats:get arena:defrag-stats)
//          + CS destruct — no UAF under ASan
//   Issue #1359 (tl_arena_capacity.cpp):
//     AC1: default capacity constant is 1MB (not 64MB)
//     AC2: OOM path does not exit — simulate via absurd capacity request
//     AC3: doubling preserved — force growth past 1MB
//     AC4 proxy: 100 threads × small alloc → each capacity stays 1MB

#include "test_harness.hpp"
#include "compiler/runtime_shared.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <print>
#include <string>
#include <thread>
#include <utility>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::SymId;
using aura::compiler::Closure;
using aura::compiler::closure_view_flat;
using aura::compiler::closure_view_owner_arena;
using aura::compiler::closure_view_pool;
using aura::compiler::ClosureView;
using aura::compiler::CompilerService;
using aura::compiler::g_closure_view_dangling_prevented_total;
using aura::compiler::g_closure_view_invalid_access_total;
using aura::compiler::invalidate_closure_lifetime;
using aura::compiler::is_closure_view_valid;
using aura::compiler::make_closure_view;
using aura::compiler::make_invalid_closure_view;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t snapshot_metric(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:closure-view-lifetime-stats\") '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

Closure make_fresh_closure(std::uint64_t version) noexcept {
    Closure cl;
    cl.name = "fn";
    cl.params = {static_cast<SymId>(1), static_cast<SymId>(2)};
    cl.body_id = 7;
    cl.lifetime_version = version;
    cl.env_id = NULL_ENV_ID;
    return cl;
}

// ── #1947/#1954 ───────────────────────────────────────────
static void ac1_stamp_and_soft_check() {
    std::println("\n--- #1947 AC1: make_closure_view stamps lifetime ---");
    Closure cl = make_fresh_closure(3);
    auto v = make_closure_view(cl);
    CHECK(v.live, "live");
    CHECK(v.source_lifetime_version == 3, "source version 3");
    CHECK(is_closure_view_valid(v), "soft check passes");
    CHECK(is_closure_view_valid(v, cl), "strong check passes");
}

static void ac2_strong_check_fails_after_tombstone() {
    std::println("\n--- #1947 AC2: tombstone → strong check fails ---");
    Closure cl = make_fresh_closure(5);
    auto v = make_closure_view(cl);
    auto before = g_closure_view_invalid_access_total.load(std::memory_order_relaxed);
    invalidate_closure_lifetime(cl);
    CHECK(!cl.lifetime_valid_for_views(), "closure tombstoned");
    CHECK(!is_closure_view_valid(v, cl), "strong check fails after tombstone");
    auto after = g_closure_view_invalid_access_total.load(std::memory_order_relaxed);
    CHECK(after == before + 1, "invalid_access_total bumped on lifetime mismatch");
}

static void ac3_make_on_tombstoned_rejects() {
    std::println("\n--- #1947 AC3: make_closure_view on tombstoned rejects ---");
    Closure cl = make_fresh_closure(7);
    invalidate_closure_lifetime(cl);
    auto before = g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed);
    auto v = make_closure_view(cl);
    CHECK(!v.live, "view not live");
    auto after = g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed);
    CHECK(after == before + 1, "dangling_prevented_total bumped on rejected make");
}

static void ac4_safe_accessors_return_null_on_invalid() {
    std::println("\n--- #1947 AC4: safe accessors return nullptr on invalid view ---");
    auto v = make_invalid_closure_view();
    auto before = g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed);
    CHECK(closure_view_flat(v) == nullptr, "flat returns nullptr");
    CHECK(closure_view_pool(v) == nullptr, "pool returns nullptr");
    CHECK(closure_view_owner_arena(v) == nullptr, "owner_arena returns nullptr");
    auto after = g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed);
    CHECK(after >= before + 3, "dangling_prevented_total bumped >=3 times");
}

static void ac5_invalid_access_counter_semantics() {
    std::println("\n--- #1947 AC5: invalid_access bumps on mismatch only ---");
    auto before = g_closure_view_invalid_access_total.load(std::memory_order_relaxed);
    Closure cl = make_fresh_closure(11);
    auto v = make_closure_view(cl);
    (void)is_closure_view_valid(v);
    auto mid = g_closure_view_invalid_access_total.load(std::memory_order_relaxed);
    CHECK(mid == before, "soft check does not bump invalid_access_total");
    (void)is_closure_view_valid(v, cl);
    auto mid2 = g_closure_view_invalid_access_total.load(std::memory_order_relaxed);
    CHECK(mid2 == mid, "strong check (matching version) does not bump invalid_access_total");
    cl.lifetime_version = 99;
    CHECK(!is_closure_view_valid(v, cl), "strong check fails on mismatch");
    auto after = g_closure_view_invalid_access_total.load(std::memory_order_relaxed);
    CHECK(after == mid2 + 1, "invalid_access_total bumped exactly once on mismatch");
}

static void ac6_concurrent_steal_gc_pressure() {
    std::println("\n--- #1947 AC6: concurrent make/invalidate/access (10k iter, 4 threads) ---");
    constexpr std::size_t kIterations = 10000;
    constexpr std::size_t kClosures = 64;
    std::vector<Closure> closures;
    closures.reserve(kClosures);
    for (std::size_t i = 0; i < kClosures; ++i) {
        Closure cl = make_fresh_closure(static_cast<std::uint64_t>(i + 1));
        cl.lifetime_valid_for_views();
        closures.push_back(cl);
    }

    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> accessor_null_count{0};
    std::atomic<std::uint64_t> strong_check_failures{0};

    auto make_thread = std::thread([&]() {
        for (std::size_t i = 0; i < kIterations; ++i) {
            auto& cl = closures[i % kClosures];
            auto v = make_closure_view(cl);
            (void)v;
        }
        done.store(true, std::memory_order_release);
    });

    auto invalidate_thread = std::thread([&]() {
        for (std::size_t i = 0; i < kIterations / 2; ++i) {
            invalidate_closure_lifetime(closures[i % kClosures]);
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });

    auto access_thread = std::thread([&]() {
        for (std::size_t i = 0; !done.load(std::memory_order_acquire); ++i) {
            auto& cl = closures[i % kClosures];
            auto v = make_closure_view(cl);
            if (closure_view_flat(v) == nullptr)
                accessor_null_count.fetch_add(1, std::memory_order_relaxed);
            if (!is_closure_view_valid(v, cl))
                strong_check_failures.fetch_add(1, std::memory_order_relaxed);
        }
    });

    make_thread.join();
    invalidate_thread.join();
    access_thread.join();

    CHECK(g_closure_view_dangling_prevented_total.load() > 0,
          "dangling_prevented_total > 0 after concurrent run");
    CHECK(g_closure_view_invalid_access_total.load() > 0,
          "invalid_access_total > 0 after concurrent run");
    CHECK(accessor_null_count.load() > 0, "at least one safe-accessor rejection observed");
    CHECK(strong_check_failures.load() > 0, "at least one strong-check failure observed");
}

// ── #300 follow-up #1 ─────────────────────────────────────
static void ac1_pcv_heap_uaf_regression() {
    std::println("\n--- #300 AC1: PCV heap UAF regression ---");
    CompilerService cs;
    cs.eval("(set-code \"(define a 1)\")");
    cs.eval("(arena:defrag)");
    auto r = cs.eval("(stats:get \"arena:defrag-stats\")");
    (void)r;
    // CS destructs at end of function. Under ASan, this used to UAF
    // (pmr::vector<PCV> realloc left aliased slots sharing one heap
    // control block with corrupted use_count → ~FlatAST double-free).
    // After fix: clean exit, no ASan report.
    std::println("  pcv heap uaf regression: no crash");
}

// ── #1359 ─────────────────────────────────────────────────
static void ac1_tl_arena_default_capacity() {
    std::println("\n--- #1359 AC1: default capacity 1MB ---");
    CHECK(TLarena::kDefaultCapacity == 1024u * 1024u, "kDefaultCapacity == 1MB");
    CHECK(aura_tl_arena_default_capacity() == TLarena::kDefaultCapacity,
          "aura_tl_arena_default_capacity probe");
    CHECK(TLarena::kDefaultCapacity < 64u * 1024u * 1024u, "default < former 64MB");

    {
        TLarena a{};
        CHECK(a.base == nullptr, "fresh base null");
        CHECK(a.capacity == 0, "fresh capacity 0 (lazy resolve)");
        CHECK(a.offset == 0, "fresh offset 0");
        CHECK(tl_arena_init(&a), "init succeeds");
        CHECK(a.base != nullptr, "init allocates base");
        CHECK(a.capacity == TLarena::kDefaultCapacity, "init capacity == 1MB");
        CHECK(a.offset == 0, "init offset 0");
        tl_arena_destroy(&a);
        CHECK(a.base == nullptr, "destroy clears base");
    }

    {
        TLarena a{};
        CHECK(tl_arena_init(&a), "init for small alloc");
        void* p = tl_arena_alloc(&a, 64, 8);
        CHECK(p != nullptr, "small alloc non-null");
        CHECK(a.capacity == TLarena::kDefaultCapacity, "small alloc keeps 1MB");
        CHECK(a.offset >= 64, "offset advanced");
        std::memset(p, 0xAB, 64);
        tl_arena_destroy(&a);
    }

    {
        TLarena a{};
        void* p = tl_arena_alloc(&a, 32, 8);
        CHECK(p != nullptr, "lazy alloc non-null");
        CHECK(a.base != nullptr, "lazy init set base");
        CHECK(a.capacity == TLarena::kDefaultCapacity, "lazy init capacity 1MB");
        tl_arena_destroy(&a);
    }
}

static void ac2_tl_arena_oom() {
    std::println("\n--- #1359 AC2: OOM path does not exit ---");
    const auto oom_before = aura_tl_arena_oom_total();
    TLarena a{};
    a.capacity = static_cast<size_t>(1) << 62;
    bool ok = tl_arena_init(&a);
    if (!ok) {
        CHECK(a.base == nullptr, "OOM init leaves base null");
        CHECK(aura_tl_arena_oom_total() > oom_before, "OOM counter incremented");
        void* p = tl_arena_alloc(&a, 16, 8);
        CHECK(p == nullptr || a.base != nullptr, "alloc either fails or recovers");
        if (a.base)
            tl_arena_destroy(&a);
    } else {
        CHECK(a.base != nullptr, "overcommit path has base");
        tl_arena_destroy(&a);
        CHECK(true, "OOM path skipped (kernel overcommit)");
    }
}

static void ac3_tl_arena_doubling() {
    std::println("\n--- #1359 AC3: doubling preserved — force growth past 1MB ---");
    {
        TLarena a{};
        CHECK(tl_arena_init(&a), "init for grow");
        const size_t big = TLarena::kDefaultCapacity + 4096;
        void* p = tl_arena_alloc(&a, big, 8);
        CHECK(p != nullptr, "grow alloc non-null");
        CHECK(a.capacity >= big, "capacity >= request");
        CHECK(a.capacity >= TLarena::kDefaultCapacity * 2, "at least one double");
        size_t cap_before = a.capacity;
        void* q = tl_arena_alloc(&a, a.capacity, 8);
        CHECK(q != nullptr, "second grow non-null");
        CHECK(a.capacity >= cap_before, "capacity non-decreasing");
        tl_arena_destroy(&a);
    }

    {
        TLarena a{};
        a.capacity = 4096;
        CHECK(tl_arena_init(&a), "init with explicit 4K");
        CHECK(a.capacity == 4096, "explicit capacity kept");
        void* p = tl_arena_alloc(&a, 64, 8);
        CHECK(p != nullptr, "alloc in 4K arena");
        void* q = tl_arena_alloc(&a, 5000, 8);
        CHECK(q != nullptr, "alloc forcing double from 4K");
        CHECK(a.capacity >= 8192, "doubled from 4K");
        tl_arena_destroy(&a);
    }

    {
        TLarena a{};
        CHECK(tl_arena_init(&a), "init push/pop");
        void* p = tl_arena_alloc(&a, 16, 8);
        CHECK(p != nullptr, "alloc before push");
        size_t off = a.offset;
        tl_arena_push(&a);
        CHECK(a.offset > off, "push advanced offset");
        tl_arena_pop(&a);
        CHECK(a.offset == off, "pop restored offset");
        tl_arena_destroy(&a);
    }
}

static void ac4_tl_arena_thread_proxy() {
    std::println("\n--- #1359 AC4 proxy: 100 threads × small alloc → 1MB each ---");
    constexpr int kThreads = 100;
    std::atomic<int> ok_count{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            TLarena local{};
            if (!tl_arena_init(&local))
                return;
            void* p = tl_arena_alloc(&local, 128, 8);
            if (!p) {
                tl_arena_destroy(&local);
                return;
            }
            CHECK(local.capacity == TLarena::kDefaultCapacity, "per-thread TLarena 1MB");
            tl_arena_destroy(&local);
            ok_count.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& t : threads)
        t.join();
    CHECK(ok_count.load() == kThreads, "100 thread-local TLarenas init+alloc+destroy");
}

} // namespace

int main() {
    std::println("=== Merged arena lifecycle: #1947/#1954 + #300 + #1359 ===");
    // #1947/#1954 (6 ACs)
    ac1_stamp_and_soft_check();
    ac2_strong_check_fails_after_tombstone();
    ac3_make_on_tombstoned_rejects();
    ac4_safe_accessors_return_null_on_invalid();
    ac5_invalid_access_counter_semantics();
    ac6_concurrent_steal_gc_pressure();
    // #300 follow-up #1 (1 AC)
    ac1_pcv_heap_uaf_regression();
    // #1359 (4 ACs)
    ac1_tl_arena_default_capacity();
    ac2_tl_arena_oom();
    ac3_tl_arena_doubling();
    ac4_tl_arena_thread_proxy();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}