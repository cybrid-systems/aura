// @category: unit
// @reason: Issue #2416 — incoming_parent_index_dirty_ is std::atomic<bool>.
//
//   AC1: flag is atomic (behavior: concurrent mark + ensure/collect)
//   AC2: concurrent add_node + collect paths do not crash / stay consistent
//   AC3: explicit load/store used (source-cite via linter)
//   AC4: dirty still triggers rebuild on next lookup

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

int main() {
    std::println("=== Issue #2416: incoming_parent_index_dirty_ atomic ===");

    // ── AC1 atomic dirty flag basic load/store ─────────────────────
    {
        std::println("\n--- #2416 AC1: atomic dirty flag basic ---");
        FlatAST flat;
        const NodeId a = flat.add_node(NodeTag::LiteralInt);
        const NodeId p = flat.add_node(NodeTag::Begin);
        flat.root = p;
        flat.insert_child(p, 0, a);
        flat.ensure_incoming_parent_index();
        CHECK(!flat.incoming_parent_index_dirty(), "AC1: clean after ensure");
        flat.mark_incoming_parent_index_dirty();
        CHECK(flat.incoming_parent_index_dirty(), "AC1: mark sets dirty");
    }

    // ── AC4 dirty triggers rebuild on next lookup ──────────────────
    {
        std::println("\n--- #2416 AC4: dirty triggers rebuild on collect ---");
        FlatAST flat;
        const NodeId a = flat.add_node(NodeTag::LiteralInt);
        const NodeId p = flat.add_node(NodeTag::Begin);
        flat.root = p;
        flat.insert_child(p, 0, a);
        flat.ensure_incoming_parent_index();
        const auto r0 = flat.incoming_parent_index_rebuilds();
        flat.mark_incoming_parent_index_dirty();
        CHECK(flat.incoming_parent_index_dirty(), "AC4: mark sets dirty");
        auto edges = flat.collect_incoming_parent_edges(a);
        CHECK(edges.size() == 1, "AC4: one parent edge after rebuild");
        CHECK(!flat.incoming_parent_index_dirty(), "AC4: clean after collect rebuild");
        CHECK(flat.incoming_parent_index_rebuilds() > r0, "AC4: rebuild counted");
    }

    // ── AC2 concurrent atomic flag + concurrent add_node ───────────
    // Rebuild/ensure walks SoA and is not concurrent-safe with add_node
    // without external serial (#2413). Stress the atomic flag itself
    // (mark + load) concurrently, and concurrent add_node writers
    // separately (flatast_mutex_ serializes).
    {
        std::println("\n--- #2416 AC2: concurrent mark/load + concurrent add_node ---");
        FlatAST flag_flat;
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> marks{0};
        std::atomic<std::uint64_t> loads{0};
        std::atomic<std::uint64_t> saw_true{0};
        std::atomic<std::uint64_t> saw_false{0};

        std::vector<std::thread> flag_threads;
        for (int t = 0; t < 4; ++t) {
            flag_threads.emplace_back([&]() {
                while (!stop.load(std::memory_order_acquire)) {
                    flag_flat.mark_incoming_parent_index_dirty();
                    marks.fetch_add(1, std::memory_order_relaxed);
                    const bool d = flag_flat.incoming_parent_index_dirty();
                    loads.fetch_add(1, std::memory_order_relaxed);
                    if (d)
                        saw_true.fetch_add(1, std::memory_order_relaxed);
                    else
                        saw_false.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        FlatAST add_flat;
        std::atomic<std::uint64_t> adds{0};
        std::vector<std::thread> add_threads;
        for (int t = 0; t < 4; ++t) {
            add_threads.emplace_back([&]() {
                while (!stop.load(std::memory_order_acquire)) {
                    (void)add_flat.add_node(NodeTag::LiteralInt);
                    adds.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        stop.store(true, std::memory_order_release);
        for (auto& th : flag_threads)
            th.join();
        for (auto& th : add_threads)
            th.join();

        std::println("  marks={} loads={} true={} false={} adds={}", marks.load(), loads.load(),
                     saw_true.load(), saw_false.load(), adds.load());
        CHECK(marks.load() > 0, "AC2: mark progressed");
        CHECK(loads.load() > 0, "AC2: load progressed");
        CHECK(saw_true.load() > 0, "AC2: observed dirty true");
        CHECK(adds.load() > 0, "AC2: concurrent add_node progressed");
        CHECK(add_flat.size() == static_cast<std::size_t>(adds.load()),
              "AC2: add_node size matches");
    }

    // ── AC3 incremental path still works when clean ────────────────
    {
        std::println("\n--- #2416 AC3: incremental insert while clean ---");
        FlatAST flat;
        const NodeId x = flat.add_node(NodeTag::LiteralInt);
        const NodeId p = flat.add_node(NodeTag::Begin);
        flat.root = p;
        flat.mark_incoming_parent_index_dirty();
        flat.ensure_incoming_parent_index();
        const auto r0 = flat.incoming_parent_index_rebuilds();
        flat.insert_child(p, 0, x);
        CHECK(!flat.incoming_parent_index_dirty(), "AC3: still clean after insert");
        auto e = flat.collect_incoming_parent_edges(x);
        CHECK(e.size() == 1, "AC3: edge visible without extra rebuild");
        CHECK(flat.incoming_parent_index_rebuilds() == r0, "AC3: no rebuild on collect");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
