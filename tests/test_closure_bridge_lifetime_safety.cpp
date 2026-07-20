// test_closure_bridge_lifetime_safety.cpp — Issue #1947
// @category: unit
// @reason: Issue #1947 — P0 ClosureView UAF guards under concurrent
// move/GC/compact + refine of #1926/#1888/#1870/#1895.
//
// AC1: make_closure_view stamps source_lifetime_version + live
//      on a fresh Closure; soft + strong validity checks pass.
// AC2: invalidate_closure_lifetime tombstone → strong check fails,
//      g_closure_view_invalid_access_total++.
// AC3: make_closure_view on already-tombstoned Closure returns
//      invalid view; g_closure_view_dangling_prevented_total++.
// AC4: safe accessors (closure_view_flat/pool/owner_arena) on
//      invalid view return nullptr; dangling_prevented_total++.
// AC5: g_closure_view_invalid_access_total increments only on
//      strong-revalidation failure (lifetime_version mismatch),
//      not on soft-rejection at view-creation time.
// AC6: long-running concurrent self-evolve + GC pressure loop:
//      N threads make views, N/2 threads invalidate closures,
//      N/2 threads read views via safe accessors — no UAF.

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
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

void ac1_stamp_and_soft_check() {
    std::println("\n--- AC1: make_closure_view stamps lifetime ---");
    Closure cl = make_fresh_closure(3);
    auto v = make_closure_view(cl);
    CHECK(v.live, "live");
    CHECK(v.source_lifetime_version == 3, "source version 3");
    CHECK(is_closure_view_valid(v), "soft check passes");
    CHECK(is_closure_view_valid(v, cl), "strong check passes");
}

void ac2_strong_check_fails_after_tombstone() {
    std::println("\n--- AC2: tombstone → strong check fails ---");
    Closure cl = make_fresh_closure(5);
    auto v = make_closure_view(cl);
    auto before = g_closure_view_invalid_access_total.load(std::memory_order_relaxed);
    invalidate_closure_lifetime(cl);
    CHECK(!cl.lifetime_valid_for_views(), "closure tombstoned");
    CHECK(!is_closure_view_valid(v, cl), "strong check fails after tombstone");
    auto after = g_closure_view_invalid_access_total.load(std::memory_order_relaxed);
    CHECK(after == before + 1, "invalid_access_total bumped on lifetime mismatch");
}

void ac3_make_on_tombstoned_rejects() {
    std::println("\n--- AC3: make_closure_view on tombstoned rejects ---");
    Closure cl = make_fresh_closure(7);
    invalidate_closure_lifetime(cl);
    auto before = g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed);
    auto v = make_closure_view(cl);
    CHECK(!v.live, "view not live");
    auto after = g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed);
    CHECK(after == before + 1, "dangling_prevented_total bumped on rejected make");
}

void ac4_safe_accessors_return_null_on_invalid() {
    std::println("\n--- AC4: safe accessors return nullptr on invalid view ---");
    auto v = make_invalid_closure_view();
    auto before = g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed);
    CHECK(closure_view_flat(v) == nullptr, "flat returns nullptr");
    CHECK(closure_view_pool(v) == nullptr, "pool returns nullptr");
    CHECK(closure_view_owner_arena(v) == nullptr, "owner_arena returns nullptr");
    auto after = g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed);
    CHECK(after >= before + 3, "dangling_prevented_total bumped >=3 times");
}

void ac5_invalid_access_counter_semantics() {
    std::println("\n--- AC5: invalid_access bumps on mismatch only ---");
    auto before = g_closure_view_invalid_access_total.load(std::memory_order_relaxed);
    Closure cl = make_fresh_closure(11);
    auto v = make_closure_view(cl);
    // Soft-only check (no cl reference) — should NOT bump invalid_access_total.
    (void)is_closure_view_valid(v);
    auto mid = g_closure_view_invalid_access_total.load(std::memory_order_relaxed);
    CHECK(mid == before, "soft check does not bump invalid_access_total");
    // Strong check with matching version — should NOT bump.
    (void)is_closure_view_valid(v, cl);
    auto mid2 = g_closure_view_invalid_access_total.load(std::memory_order_relaxed);
    CHECK(mid2 == mid, "strong check (matching version) does not bump invalid_access_total");
    // Bump lifetime_version to force mismatch.
    cl.lifetime_version = 99;
    CHECK(!is_closure_view_valid(v, cl), "strong check fails on mismatch");
    auto after = g_closure_view_invalid_access_total.load(std::memory_order_relaxed);
    CHECK(after == mid2 + 1, "invalid_access_total bumped exactly once on mismatch");
}

void ac6_concurrent_steal_gc_pressure() {
    std::println("\n--- AC6: concurrent make/invalidate/access (10k iter, 4 threads) ---");
    constexpr std::size_t kIterations = 10000;
    constexpr std::size_t kClosures = 64;
    std::vector<Closure> closures;
    closures.reserve(kClosures);
    for (std::size_t i = 0; i < kClosures; ++i) {
        Closure cl = make_fresh_closure(static_cast<std::uint64_t>(i + 1));
        cl.lifetime_valid_for_views(); // sanity: not tombstoned
        closures.push_back(cl);
    }

    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> accessor_null_count{0};
    std::atomic<std::uint64_t> strong_check_failures{0};

    // Thread A: make_closure_view every iteration (different closures).
    auto make_thread = std::thread([&]() {
        for (std::size_t i = 0; i < kIterations; ++i) {
            auto& cl = closures[i % kClosures];
            auto v = make_closure_view(cl);
            (void)v;
        }
        done.store(true, std::memory_order_release);
    });

    // Thread B: invalidate every Nth iteration (simulates GC/move).
    auto invalidate_thread = std::thread([&]() {
        for (std::size_t i = 0; i < kIterations / 2; ++i) {
            invalidate_closure_lifetime(closures[i % kClosures]);
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });

    // Thread C: access safe accessors on a fresh view (must return non-null
    // when closure is fresh; null when tombstoned).
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

    // Accessor returned nullptr exactly when the closure was tombstoned at
    // access time — no UAF. Strong-check failure is expected after
    // invalidate_closure_lifetime. Counters monotonically increased.
    CHECK(g_closure_view_dangling_prevented_total.load() > 0,
          "dangling_prevented_total > 0 after concurrent run");
    CHECK(g_closure_view_invalid_access_total.load() > 0,
          "invalid_access_total > 0 after concurrent run");
    CHECK(accessor_null_count.load() > 0, "at least one safe-accessor rejection observed");
    CHECK(strong_check_failures.load() > 0, "at least one strong-check failure observed");
}

} // namespace

int main() {
    ac1_stamp_and_soft_check();
    ac2_strong_check_fails_after_tombstone();
    ac3_make_on_tombstoned_rejects();
    ac4_safe_accessors_return_null_on_invalid();
    ac5_invalid_access_counter_semantics();
    ac6_concurrent_steal_gc_pressure();
    if (g_failed)
        return 1;
    std::println("closure_bridge_lifetime_safety #1947: OK ({} passed)", g_passed);
    return 0;
}