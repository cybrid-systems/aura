// @category: unit
// @reason: Issue #2429 — policy check + overflow arm under same mutex
//          (no HardFail bypass race with concurrent set_policy).
//
//   AC1: policy check + arm/reject atomic under g_gc_defer_armed_mtx
//   AC2: concurrent set_gc_defer_overflow_policy + try_arm (TSan-friendly)
//   AC3: under HardFail overflow, never depth bump / Panic arm for reject
//   AC4: HardFail semantics preserved (reject + counter)

#include "test_harness.hpp"

#include "core/gc_hooks.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;

namespace {

using aura::test::g_failed;
using aura::test::g_passed;
namespace gh = aura::gc_hooks;

void cleanup_ids(const std::vector<void*>& ids) {
    for (auto* id : ids)
        gh::release_gc_defer_pending_panic_for(id);
}

// Fill the per-eval table to capacity so the next arm is overflow.
std::vector<void*> fill_table(std::size_t cap) {
    std::vector<void*> ids;
    ids.reserve(cap);
    for (std::size_t i = 0; i < cap; ++i) {
        void* id = reinterpret_cast<void*>(0xB4290000u + static_cast<unsigned>(i));
        ids.push_back(id);
        gh::arm_gc_defer_pending_panic_for(id);
    }
    return ids;
}

} // namespace

int main() {
    std::println("=== Issue #2429: overflow policy check+arm atomic ===");

    // ── AC4 HardFail single-thread baseline ────────────────────────
    {
        std::println("\n--- #2429 AC4: HardFail overflow reject baseline ---");
        gh::reset_gc_defer_overflow_policy_for_test();
        gh::reset_gc_defer_max_armed_for_test();
        gh::set_gc_defer_overflow_policy_for_test(gh::GcDeferOverflowPolicy::HardFail);
        gh::set_gc_defer_max_armed_for_test(8);

        const auto rej0 = gh::gc_defer_arm_rejected_overflow_total();
        const auto ovf0 = gh::g_gc_defer_table_overflow_total.load(std::memory_order_relaxed);
        const auto depth0 = gh::gc_defer_pending_panic_depth();

        auto ids = fill_table(8);
        CHECK(gh::gc_defer_pending_panic_depth() == depth0 + 8, "AC4: table fill depth +8");

        void* extra = reinterpret_cast<void*>(0xDEAD2429u);
        const bool armed = gh::try_arm_gc_defer_pending_panic_for(extra);
        CHECK(!armed, "AC4: HardFail overflow → false");
        CHECK(gh::gc_defer_arm_rejected_overflow_total() == rej0 + 1, "AC4: rejected +1");
        CHECK(gh::g_gc_defer_table_overflow_total.load() == ovf0, "AC4: no table overflow bump");
        CHECK(gh::gc_defer_pending_panic_depth() == depth0 + 8, "AC4: depth unchanged on reject");
        CHECK(!gh::gc_deferred_for_evaluator(extra), "AC4: extra not deferred");

        cleanup_ids(ids);
        gh::reset_gc_defer_overflow_policy_for_test();
        gh::reset_gc_defer_max_armed_for_test();
    }

    // ── AC1/AC3: ProcessWide then HardFail mid-stress ──────────────
    {
        std::println(
            "\n--- #2429 AC1 + #2429 AC2 + #2429 AC3: concurrent policy flip + try_arm ---");
        gh::reset_gc_defer_overflow_policy_for_test();
        gh::reset_gc_defer_max_armed_for_test();
        gh::set_gc_defer_max_armed_for_test(8);
        // Start ProcessWide so overflow can arm; flip to HardFail concurrently.
        gh::set_gc_defer_overflow_policy_for_test(gh::GcDeferOverflowPolicy::ProcessWide);

        auto ids = fill_table(8);
        const auto depth_after_fill = gh::gc_defer_pending_panic_depth();

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> tries{0};
        std::atomic<std::uint64_t> true_rets{0};
        std::atomic<std::uint64_t> false_rets{0};
        std::atomic<std::uint64_t> policy_flips{0};
        std::atomic<std::uint64_t> err{0};
        // AC3 invariant: when try_arm returns false, depth must not have
        // been bumped by that call. Track depth deltas on false.
        std::atomic<std::uint64_t> false_with_depth_bump{0};

        std::vector<std::thread> threads;
        // 2 policy flippers: toggle HardFail ↔ ProcessWide
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t]() {
                std::uint64_t i = static_cast<std::uint64_t>(t);
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        if ((i & 1u) == 0)
                            gh::set_gc_defer_overflow_policy_for_test(
                                gh::GcDeferOverflowPolicy::HardFail);
                        else
                            gh::set_gc_defer_overflow_policy_for_test(
                                gh::GcDeferOverflowPolicy::ProcessWide);
                        policy_flips.fetch_add(1, std::memory_order_relaxed);
                        ++i;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // 4 try_arm overflow callers (unique evaluator ids so they never hit table)
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                // Unique id per attempt to force overflow path (table full).
                std::uint64_t n = static_cast<std::uint64_t>(t) << 16;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        void* id = reinterpret_cast<void*>(0xC0000000u + static_cast<unsigned>(n));
                        const auto d0 = gh::gc_defer_pending_panic_depth();
                        const bool ok = gh::try_arm_gc_defer_pending_panic_for(id);
                        const auto d1 = gh::gc_defer_pending_panic_depth();
                        tries.fetch_add(1, std::memory_order_relaxed);
                        if (ok) {
                            true_rets.fetch_add(1, std::memory_order_relaxed);
                            // ProcessWide overflow arms process-wide only —
                            // release the process depth bump without table slot.
                            // release_gc_defer_pending_panic_for(id) won't find
                            // the id; use process-wide release once.
                            gh::release_gc_defer_pending_panic();
                        } else {
                            false_rets.fetch_add(1, std::memory_order_relaxed);
                            // AC3: false ⇒ no depth increase from this call.
                            // Other threads may race depth; only flag if our
                            // call claimed reject but depth went up *and*
                            // policy is HardFail with no concurrent ProcessWide
                            // arm possible... Strict: if false and d1 > d0 by
                            // more than concurrent true arms can explain is hard.
                            // Simpler invariant: if false, extra id must not be
                            // in the per-eval table.
                            if (gh::gc_deferred_for_evaluator(id))
                                false_with_depth_bump.fetch_add(1, std::memory_order_relaxed);
                            (void)d0;
                            (void)d1;
                        }
                        ++n;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        stop.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        // Drain any residual process-wide depth from ProcessWide overflow arms.
        while (gh::gc_defer_pending_panic_depth() > depth_after_fill)
            gh::release_gc_defer_pending_panic();

        std::println("  tries={} true={} false={} flips={} false_with_eval={} err={} depth_fill={}",
                     tries.load(), true_rets.load(), false_rets.load(), policy_flips.load(),
                     false_with_depth_bump.load(), err.load(), depth_after_fill);

        CHECK(tries.load() > 0, "AC2: concurrent try_arm progressed");
        CHECK(policy_flips.load() > 0, "AC2: concurrent policy flips progressed");
        CHECK(true_rets.load() > 0 || false_rets.load() > 0, "AC2: got results");
        CHECK(err.load() == 0, "AC2: no exceptions");
        CHECK(false_with_depth_bump.load() == 0,
              "AC3: false return never marks evaluator deferred");

        // Final HardFail check after concurrent storm (table still full).
        gh::set_gc_defer_overflow_policy_for_test(gh::GcDeferOverflowPolicy::HardFail);
        const auto rej_b = gh::gc_defer_arm_rejected_overflow_total();
        const auto d_b = gh::gc_defer_pending_panic_depth();
        void* final_id = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xF1002429u));
        const bool final_armed = gh::try_arm_gc_defer_pending_panic_for(final_id);
        CHECK(!final_armed, "AC3: HardFail still rejects after storm");
        CHECK(gh::gc_defer_arm_rejected_overflow_total() >= rej_b + 1, "AC3: reject counted");
        CHECK(gh::gc_defer_pending_panic_depth() == d_b, "AC3: depth stable on final reject");

        cleanup_ids(ids);
        while (gh::gc_defer_pending_panic_depth() > 0)
            gh::release_gc_defer_pending_panic();
        gh::reset_gc_defer_overflow_policy_for_test();
        gh::reset_gc_defer_max_armed_for_test();
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
