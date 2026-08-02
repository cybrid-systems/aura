// @category: unit
// @reason: Issue #2413 — FlatAST add_node multi-column SoA lock contract.
//
//   AC1: class contract documents flatast_mutex_ reader invariant
//   AC2: audit findings recorded (check script + follow-up issue)
//   AC3: concurrent writers (add_node×N) serialize; full init after return
//   AC4: no behavior change for single-threaded callers (spot-check)

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
using aura::ast::NodeTag;
using aura::test::g_failed;
using aura::test::g_passed;

} // namespace

int run_test_flatast_add_node_lock_2413() {
    std::println("=== Issue #2413: FlatAST add_node SoA lock contract ===");

    // ── AC1 source-cite: contract strings live in ast.ixx (linter) ─
    {
        std::println("\n--- #2413 AC1: documented concurrent access contract ---");
        // Behavioral anchor: add_node returns only after all columns exist.
        FlatAST flat;
        const auto id = flat.add_node(NodeTag::LiteralInt);
        CHECK(id < flat.size(), "AC1: id in range after add_node");
        CHECK(flat.tag(id) == NodeTag::LiteralInt, "AC1: tag published");
        CHECK(flat.int_val(id) == 0, "AC1: int_val zero-init");
        CHECK(flat.sym_id(id) == aura::ast::INVALID_SYM, "AC1: sym published");
        const auto v = flat.get(id);
        CHECK(v.tag == NodeTag::LiteralInt, "AC1: get() multi-column consistent");
        CHECK(v.int_value == 0, "AC1: get int zero");
    }

    // ── AC2 audit note (runtime sanity of workspace-style path) ────
    {
        std::println("\n--- #2413 AC2: single-thread multi-column after recycle ---");
        FlatAST flat;
        const auto a = flat.add_node(NodeTag::Begin);
        flat.root = a;
        const auto b = flat.add_node(NodeTag::LiteralInt);
        flat.insert_child(a, 0, b);
        (void)flat.recycle_dead_nodes(); // no-op if live
        // Free b, recycle, re-add — reset_node_slot also under mutex.
        flat.remove_child(a, 0);
        (void)flat.recycle_dead_nodes();
        const auto c = flat.add_node(NodeTag::Variable);
        CHECK(flat.tag(c) == NodeTag::Variable, "AC2: recycled slot fully tagged");
        CHECK(flat.int_val(c) == 0, "AC2: recycled int zero");
        CHECK(flat.parent_of(c) == aura::ast::NULL_NODE, "AC2: parent cleared");
    }

    // ── AC3 concurrent add_node writers serialize ──────────────────
    {
        std::println("\n--- #2413 AC3: concurrent add_node writers serialize ---");
        FlatAST flat;
        constexpr int kThreads = 4;
        constexpr int kPer = 200;
        std::atomic<int> started{0};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&flat, &started]() {
                started.fetch_add(1, std::memory_order_relaxed);
                for (int i = 0; i < kPer; ++i)
                    (void)flat.add_node(NodeTag::LiteralInt);
            });
        }
        for (auto& th : threads)
            th.join();
        CHECK(started.load() == kThreads, "AC3: all threads ran");
        CHECK(flat.size() == static_cast<std::size_t>(kThreads * kPer),
              "AC3: size == sum of adds (no lost/duplicate slots)");
        // Spot-check a sample of nodes are fully initialized.
        for (std::size_t i = 0; i < flat.size(); i += 50) {
            CHECK(flat.tag(static_cast<aura::ast::NodeId>(i)) == NodeTag::LiteralInt,
                  "AC3: tag intact after concurrent adds");
            CHECK(flat.int_val(static_cast<aura::ast::NodeId>(i)) == 0, "AC3: int_val intact");
        }
    }

    // ── AC4 single-thread behavior unchanged ───────────────────────
    {
        std::println("\n--- #2413 AC4: single-thread add_node unchanged ---");
        FlatAST flat;
        const auto x = flat.add_node(NodeTag::Call);
        const auto y = flat.add_node(NodeTag::Lambda);
        CHECK(flat.size() == 2, "AC4: size 2");
        CHECK(flat.tag(x) == NodeTag::Call, "AC4: Call");
        CHECK(flat.tag(y) == NodeTag::Lambda, "AC4: Lambda");
        flat.clear();
        CHECK(flat.size() == 0, "AC4: clear empties");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_flatast_add_node_lock_2413();
}
#endif
