// @category: unit
// @reason: Issue #2424 — is_subtree_dirty_node bounds via dirty_.size()
//          (not size()/tag_); safe under concurrent add_node.
//
//   AC1: no OOB on dirty_ (bounds use dirty_.size() only)
//   AC2: concurrent is_subtree_dirty_node + add_node (TSan-friendly)
//   AC3: return semantics unchanged for non-racing callers
//   AC4: add_node documents dirty_.size() == tag_.size() invariant

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
using aura::ast::NULL_NODE;
using aura::test::g_failed;
using aura::test::g_passed;

} // namespace

int run_test_subtree_dirty_bounds() {
    std::println("=== Issue #2424: is_subtree_dirty_node dirty_.size() bounds ===");

    // ── AC3 non-racing semantics ───────────────────────────────────
    {
        std::println("\n--- #2424 AC3: non-racing return semantics ---");
        FlatAST flat;
        CHECK(!flat.is_subtree_dirty_node(NULL_NODE), "AC3: NULL_NODE → false");
        CHECK(!flat.is_subtree_dirty_node(0), "AC3: empty AST id 0 → false");
        const auto a = flat.add_node(NodeTag::LiteralInt);
        const auto b = flat.add_node(NodeTag::LiteralInt);
        CHECK(!flat.is_subtree_dirty_node(a), "AC3: fresh node clean");
        CHECK(!flat.is_subtree_dirty_node(b), "AC3: fresh node b clean");
        flat.mark_dirty(a);
        CHECK(flat.is_subtree_dirty_node(a), "AC3: marked a dirty");
        CHECK(!flat.is_subtree_dirty_node(b), "AC3: b still clean");
        // OOB / not-yet-grown ids: false, not crash
        CHECK(!flat.is_subtree_dirty_node(static_cast<NodeId>(flat.size() + 100)),
              "AC3: far OOB → false");
        CHECK(flat.dirty_nodes_in_range(0, flat.size()) == 1, "AC3: range count 1");
    }

    // ── AC1/AC2 concurrent add_node + is_subtree_dirty_node ────────
    {
        std::println("\n--- #2424 AC1 + #2424 AC2: concurrent add_node + is_subtree_dirty ---");
        FlatAST flat;
        // Seed a few nodes so readers have published ids.
        for (int i = 0; i < 32; ++i)
            (void)flat.add_node(NodeTag::LiteralInt);

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> add_ops{0};
        std::atomic<std::uint64_t> query_ops{0};
        std::atomic<std::uint64_t> range_ops{0};
        std::atomic<std::uint64_t> err{0};
        std::atomic<std::uint64_t> dirty_true{0};

        std::vector<std::thread> threads;
        // 2 add_node writers
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&]() {
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        (void)flat.add_node(NodeTag::LiteralInt);
                        add_ops.fetch_add(1, std::memory_order_relaxed);
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // 2 is_subtree_dirty_node readers over growing range
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        // Probe ids that may race with growth (including past size).
                        const auto probe = static_cast<NodeId>(i % 4096);
                        if (flat.is_subtree_dirty_node(probe))
                            dirty_true.fetch_add(1, std::memory_order_relaxed);
                        query_ops.fetch_add(1, std::memory_order_relaxed);
                        i += 2;
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // 1 dirty_nodes_in_range reader
        threads.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    (void)flat.dirty_nodes_in_range(0, 4096);
                    range_ops.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    err.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        // 1 mark_dirty writer on low ids (exercises exclusive vs add_node)
        threads.emplace_back([&]() {
            int i = 0;
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    const auto n = flat.size();
                    if (n > 0) {
                        const auto id = static_cast<NodeId>(i % static_cast<int>(n));
                        flat.mark_dirty(id);
                    }
                    i++;
                } catch (...) {
                    err.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        stop.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        std::println("  add_ops={} query_ops={} range_ops={} dirty_true={} err={} size={}",
                     add_ops.load(), query_ops.load(), range_ops.load(), dirty_true.load(),
                     err.load(), flat.size());
        CHECK(add_ops.load() > 0, "AC2: concurrent add_node progressed");
        CHECK(query_ops.load() > 0, "AC2: concurrent is_subtree_dirty_node progressed");
        CHECK(range_ops.load() > 0, "AC2: concurrent dirty_nodes_in_range progressed");
        CHECK(err.load() == 0, "AC1/AC2: no exceptions (no OOB crash)");
        CHECK(flat.size() >= 32 + add_ops.load(), "AC2: size grew with adds");
    }

    // ── AC4 invariant after growth (source-cited + runtime check) ──
    {
        std::println("\n--- #2424 AC4: dirty_.size() tracks growth via APIs ---");
        FlatAST flat;
        for (int i = 0; i < 50; ++i)
            (void)flat.add_node(NodeTag::LiteralInt);
        // After add_node, every in-range id is queryable (not OOB).
        for (NodeId id = 0; id < flat.size(); ++id)
            (void)flat.is_subtree_dirty_node(id);
        CHECK(flat.dirty_nodes_in_range(0, flat.size()) == 0, "AC4: all clean after adds");
        flat.mark_dirty(0);
        flat.mark_dirty(flat.size() - 1);
        CHECK(flat.dirty_nodes_in_range(0, flat.size()) == 2, "AC4: both ends markable");
        CHECK(flat.is_subtree_dirty_node(0), "AC4: first dirty");
        CHECK(flat.is_subtree_dirty_node(flat.size() - 1), "AC4: last dirty");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_subtree_dirty_bounds();
}
#endif
