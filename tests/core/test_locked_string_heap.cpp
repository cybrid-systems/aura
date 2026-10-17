// @category: unit
// @reason: Issue #4543 — string_heap_ growth only through the locked mint API
//
// #2651 requires every Evaluator::string_heap_ growth to be serialized behind
// alloc_storage_lock_. ~650 primitive sites minted with an unlocked
// `idx = heap.size(); heap.push_back(s);` pair (#4519 AC2a TSan: unlocked
// writers racing locked pushers under fiber:spawn sharing → free(): invalid
// pointer / 0xbe-fill teardown SEGV). The heap is now a LockedStringHeap.
//
//   AC1: compile-time — LockedStringHeap has no push_back / emplace_back /
//        insert; Evaluator::string_heap_mut() hands out that type, so an
//        unlocked mint does not compile.
//   AC2: N threads mint into one bound heap: indices are unique and dense,
//        every slot holds the string its minter wrote, and the mint hook ran
//        once per index under the lock (TSan-clean).
//   AC3: N threads call string-minting primitives from swept files on one
//        shared Evaluator: every result string is intact and the indices are
//        pairwise distinct (the old size()+push_back pair handed two callers
//        the same slot). Run under --sanitizer=tsan for the race face.
//   AC4: the #4428 tenant stamp is written for primitive mints (the hook
//        replaces the per-site note_inrange_string_mint calls).

#include "test_harness.hpp"
#include "compiler/locked_string_heap.hh"

#include <atomic>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <print>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::LockedStringHeap;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_string;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

// ── AC1: the unlocked growth surface does not exist ──
template <class H>
concept HasPushBack = requires(H& h) { h.push_back(std::string{}); };
template <class H>
concept HasEmplaceBack = requires(H& h) { h.emplace_back("x"); };
template <class H>
concept HasInsert = requires(H& h) { h.insert(h.end(), std::string{}); };
template <class H>
concept HasMint = requires(H& h) {
    { h.mint(std::string{}) } -> std::same_as<std::uint64_t>;
};
static_assert(!HasPushBack<LockedStringHeap>, "#4543: no unlocked push_back");
static_assert(!HasEmplaceBack<LockedStringHeap>, "#4543: no unlocked emplace_back");
static_assert(!HasInsert<LockedStringHeap>, "#4543: no unlocked insert");
static_assert(HasMint<LockedStringHeap>, "#4543: mint is the growth API");
static_assert(!std::is_copy_constructible_v<LockedStringHeap>);
static_assert(
    std::is_same_v<std::remove_cvref_t<
                       decltype(std::declval<CompilerService&>().evaluator().string_heap_mut())>,
                   LockedStringHeap>,
    "#4543: Evaluator hands out the locked heap");

struct HookLog {
    std::vector<std::uint64_t> seen; // written only under the heap lock
};

void ac2_concurrent_mint() {
    std::println("\n--- AC2: concurrent mint into one bound heap ---");
    std::pmr::monotonic_buffer_resource res;
    std::recursive_mutex mu;
    LockedStringHeap heap(&res, &mu);
    HookLog log;
    heap.set_mint_hook(
        [](void* ctx, std::size_t idx) noexcept {
            static_cast<HookLog*>(ctx)->seen.push_back(idx);
        },
        &log);
    constexpr int kThreads = 8;
    constexpr int kPer = 4000;
    std::vector<std::vector<std::pair<std::uint64_t, std::string>>> got(kThreads);
    std::atomic<int> go{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            while (go.load(std::memory_order_acquire) == 0) {
            }
            got[t].reserve(kPer);
            for (int i = 0; i < kPer; ++i) {
                std::string s = "t" + std::to_string(t) + ":" + std::to_string(i) +
                                std::string(static_cast<std::size_t>(i % 40), 'x');
                const auto idx = heap.mint(s);
                got[t].emplace_back(idx, std::move(s));
            }
        });
    }
    go.store(1, std::memory_order_release);
    for (auto& th : ts)
        th.join();
    const std::size_t total = static_cast<std::size_t>(kThreads) * kPer;
    CHECK(heap.size() == total, "AC2: heap size == total mints");
    std::vector<char> hit(total, 0);
    bool unique = true, intact = true;
    for (const auto& per : got) {
        for (const auto& [idx, s] : per) {
            if (idx >= total || hit[idx]) {
                unique = false;
                continue;
            }
            hit[idx] = 1;
            if (heap[idx] != s)
                intact = false;
        }
    }
    CHECK(unique, "AC2: every minted index is unique and in range");
    CHECK(intact, "AC2: every slot holds its minter's string");
    CHECK(log.seen.size() == total, "AC2: hook ran once per mint");
    bool ordered = true;
    for (std::size_t i = 0; i < log.seen.size(); ++i)
        if (log.seen[i] != i)
            ordered = false;
    CHECK(ordered, "AC2: hook saw indices in mint order (ran inside the critical section)");
}

void ac3_shared_evaluator_prims() {
    std::println("\n--- AC3: string-minting prims on one shared Evaluator ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    // obs_eval (panic-safe-source, the largest swept file) and pair
    // (number->string). Neither touches pairs_, so concurrent calls
    // exercise the string heap only. (query:* stats are not in the
    // primitive registry, so they are not reachable via lookup.)
    auto pss = ev.primitives().lookup("panic-safe-source");
    auto n2s = ev.primitives().lookup("number->string");
    CHECK(pss.has_value() && n2s.has_value(), "AC3: prims registered");
    if (!pss || !n2s)
        return;
    constexpr int kThreads = 6;
    constexpr int kPer = 1500;
    const std::size_t heap0 = ev.string_heap_size();
    std::vector<std::vector<std::uint64_t>> idxs(kThreads);
    std::atomic<int> bad{0};
    std::atomic<int> go{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            while (go.load(std::memory_order_acquire) == 0) {
            }
            idxs[t].reserve(kPer);
            for (int i = 0; i < kPer; ++i) {
                const EvalValue argv[1] = {make_int(i)};
                auto r =
                    ((t + i) % 2 == 0) ? (*pss)({}) : (*n2s)(std::span<const EvalValue>(argv, 1));
                if (!is_string(r)) {
                    bad.fetch_add(1);
                    continue;
                }
                idxs[t].push_back(as_string_idx(r));
            }
        });
    }
    go.store(1, std::memory_order_release);
    for (auto& th : ts)
        th.join();
    CHECK(bad.load() == 0, "AC3: every call returned a string");
    std::set<std::uint64_t> all;
    std::size_t n = 0;
    for (const auto& v : idxs) {
        for (auto i : v) {
            all.insert(i);
            ++n;
        }
    }
    CHECK(all.size() == n, "AC3: no two callers were handed the same heap slot");
    CHECK(ev.string_heap_size() == heap0 + n, "AC3: heap grew by exactly one slot per call");
    bool in_range = true;
    for (auto i : all)
        if (i < heap0 || i >= ev.string_heap_size())
            in_range = false;
    CHECK(in_range, "AC3: every index lands in the newly minted range");
}

void ac4_tenant_stamp_on_prim_mint() {
    std::println("\n--- AC4: #4428 stamp written for a primitive mint ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto pss = ev.primitives().lookup("panic-safe-source");
    CHECK(pss.has_value(), "AC4: panic-safe-source registered");
    if (!pss)
        return;
    auto r = (*pss)({});
    CHECK(is_string(r), "AC4: returns a string");
    if (!is_string(r))
        return;
    const auto idx = as_string_idx(r);
    CHECK(ev.string_slot_tenant_record_for_test(idx) == ev.capability_tenant_id() + 1,
          "AC4: minted slot carries the minting tenant's stamp");
}

} // namespace

int main() {
    std::println("=== test_locked_string_heap (#4543) ===");
    ac2_concurrent_mint();
    ac3_shared_evaluator_prims();
    ac4_tenant_stamp_on_prim_mint();
    std::println("\n=== test_locked_string_heap: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
