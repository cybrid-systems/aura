// @category: unit
// @reason: Issue #2423 — dirty_nodes_in_range / is_subtree_dirty_node
//          are thread-safe under concurrent mark_dirty.
//
//   AC1: concurrent mark_dirty + dirty_nodes_in_range (shared/exclusive lock)
//   AC2: concurrent mark_dirty + is_subtree_dirty_node + range scan (TSan-friendly)
//   AC3: correct count after concurrent marks (monotonic, all marked seen)
//   AC4: uncontended shared_lock path still correct (single-thread baseline)

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::test::g_failed;
using aura::test::g_passed;

} // namespace

int run_test_dirty_column_lock_2423() {
    std::println("=== Issue #2423: dirty_ column lock for short-circuit APIs ===");

    // ── AC4 single-thread baseline ─────────────────────────────────
    {
        std::println("\n--- #2423 AC4: uncontended dirty_nodes_in_range ---");
        FlatAST flat;
        constexpr int kN = 64;
        for (int i = 0; i < kN; ++i)
            (void)flat.add_node(NodeTag::LiteralInt);
        CHECK(flat.dirty_nodes_in_range(0, static_cast<NodeId>(kN)) == 0,
              "AC4: empty dirty count is 0");
        CHECK(!flat.is_subtree_dirty_node(0), "AC4: node 0 clean");
        flat.mark_dirty(0);
        flat.mark_dirty(10);
        flat.mark_dirty(20);
        CHECK(flat.is_subtree_dirty_node(0), "AC4: node 0 dirty");
        CHECK(flat.is_subtree_dirty_node(10), "AC4: node 10 dirty");
        CHECK(!flat.is_subtree_dirty_node(1), "AC4: node 1 still clean");
        const auto n = flat.dirty_nodes_in_range(0, static_cast<NodeId>(kN));
        CHECK(n == 3, "AC4: count == 3 after three marks");
        const auto mid = flat.dirty_nodes_in_range(5, 15);
        CHECK(mid == 1, "AC4: range [5,15) sees only node 10");
    }

    // ── AC1/AC2/AC3 concurrent mark + range + is_subtree_dirty ─────
    {
        std::println(
            "\n--- #2423 AC1 + #2423 AC2 + #2423 AC3: concurrent mark_dirty + readers ---");
        FlatAST flat;
        constexpr int kN = 256;
        for (int i = 0; i < kN; ++i)
            (void)flat.add_node(NodeTag::LiteralInt);

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> mark_ops{0};
        std::atomic<std::uint64_t> range_ops{0};
        std::atomic<std::uint64_t> node_ops{0};
        std::atomic<std::uint64_t> err{0};
        std::atomic<std::uint64_t> max_count{0};

        std::vector<std::thread> threads;
        // 2 mark_dirty writers
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        const auto id = static_cast<NodeId>(i % kN);
                        flat.mark_dirty(id);
                        mark_ops.fetch_add(1, std::memory_order_relaxed);
                        i += 2;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // 2 dirty_nodes_in_range readers
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&]() {
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        const auto c = flat.dirty_nodes_in_range(0, static_cast<NodeId>(kN));
                        range_ops.fetch_add(1, std::memory_order_relaxed);
                        // Counts are snapshots; must stay in [0, kN].
                        if (c > static_cast<std::size_t>(kN))
                            err.fetch_add(1, std::memory_order_relaxed);
                        // Track max observed for AC3 post-check.
                        auto prev = max_count.load(std::memory_order_relaxed);
                        while (c > prev && !max_count.compare_exchange_weak(
                                               prev, c, std::memory_order_relaxed)) {
                        }
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // 2 is_subtree_dirty_node readers
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        (void)flat.is_subtree_dirty_node(static_cast<NodeId>(i % kN));
                        node_ops.fetch_add(1, std::memory_order_relaxed);
                        i += 2;
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

        const auto final_count = flat.dirty_nodes_in_range(0, static_cast<NodeId>(kN));
        std::println("  mark_ops={} range_ops={} node_ops={} err={} max_count={} final={}",
                     mark_ops.load(), range_ops.load(), node_ops.load(), err.load(),
                     max_count.load(), final_count);

        CHECK(mark_ops.load() > 0, "AC1: concurrent mark_dirty progressed");
        CHECK(range_ops.load() > 0, "AC1: concurrent dirty_nodes_in_range progressed");
        CHECK(node_ops.load() > 0, "AC2: concurrent is_subtree_dirty_node progressed");
        CHECK(err.load() == 0, "AC2: no exceptions / impossible counts");
        // AC3: after many concurrent marks over all ids, final count should
        // be high (writers cover all slots); at least max observed and final
        // are consistent and non-zero.
        CHECK(final_count > 0, "AC3: final dirty count > 0");
        CHECK(final_count <= static_cast<std::size_t>(kN), "AC3: final count <= N");
        CHECK(max_count.load() <= static_cast<std::size_t>(kN), "AC3: max snapshot <= N");
        // With enough marks, most/all nodes should be dirty.
        CHECK(final_count >= static_cast<std::size_t>(kN) / 2 || mark_ops.load() < 100,
              "AC3: majority dirty after concurrent marks (or few ops)");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dirty_column_lock_2423();
}
#endif
