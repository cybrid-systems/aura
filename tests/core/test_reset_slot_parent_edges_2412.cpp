// @category: unit
// @reason: Issue #2412 — reset_node_slot always clears incoming_parent_edges_.
//
//   AC1: edges empty after every reset, even when index is dirty
//   AC2: collect_incoming_parent_edges still correct after rebuild
//   AC3: 2nd recycle of the same slot also starts with empty edges
//   AC4: ASan/TSan clean (no use-after-free / OOB)

#include "test_harness.hpp"

#include <cstdint>
#include <print>

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

int main() {
    std::println("=== Issue #2412: reset_node_slot clears incoming edges ===");

    // ── AC1 dirty-index recycle still clears edges ─────────────────
    {
        std::println("\n--- #2412 AC1: clear even when index dirty ---");
        FlatAST flat;
        const NodeId child = flat.add_node(NodeTag::LiteralInt);
        const NodeId parent = flat.add_node(NodeTag::Begin);
        flat.root = parent;
        flat.insert_child(parent, 0, child);
        flat.ensure_incoming_parent_index();
        CHECK(!flat.incoming_parent_index_dirty(), "AC1: index clean after ensure");
        CHECK(flat.incoming_parent_edge_count_raw(child) == 1, "AC1: one edge before free");

        // Dirty so remove_child does not touch inverted edges (stale remain).
        flat.mark_incoming_parent_index_dirty();
        CHECK(flat.incoming_parent_index_dirty(), "AC1: dirty set");
        flat.remove_child(parent, 0);
        CHECK(flat.incoming_parent_edge_count_raw(child) == 1,
              "AC1: dirty remove leaves raw edges (precondition)");

        const auto recycled = flat.recycle_dead_nodes();
        CHECK(recycled >= 1, "AC1: recycled orphan child");
        CHECK(flat.is_free_slot(child), "AC1: child free");

        // Reuse free-list slot → reset_node_slot (dirty still true).
        const NodeId reused = flat.add_node(NodeTag::LiteralInt);
        CHECK(reused == child, "AC1: free-list reuses same id");
        CHECK(flat.incoming_parent_edge_count_raw(reused) == 0,
              "AC1: edges empty after reset despite dirty");
        CHECK(flat.incoming_parent_index_dirty(), "AC1: dirty flag unchanged by reset");
    }

    // ── AC2 collect/rebuild still correct ──────────────────────────
    {
        std::println("\n--- #2412 AC2: collect after rebuild still correct ---");
        FlatAST flat;
        const NodeId a = flat.add_node(NodeTag::LiteralInt);
        const NodeId b = flat.add_node(NodeTag::LiteralInt);
        const NodeId p1 = flat.add_node(NodeTag::Begin);
        const NodeId p2 = flat.add_node(NodeTag::Begin);
        flat.root = p1;
        flat.insert_child(p1, 0, a);
        flat.insert_child(p1, 1, b);
        flat.insert_child(p2, 0, a);
        flat.mark_incoming_parent_index_dirty();
        auto edges = flat.collect_incoming_parent_edges(a);
        CHECK(edges.size() == 2, "AC2: a has two parents after rebuild");
        CHECK(!flat.incoming_parent_index_dirty(), "AC2: clean after collect");
        // Drop b, recycle, reuse — collect on a still 2 (b gone doesn't affect a)
        flat.remove_child(p1, 1);
        (void)flat.recycle_dead_nodes();
        (void)flat.add_node(NodeTag::LiteralInt);
        edges = flat.collect_incoming_parent_edges(a);
        CHECK(edges.size() == 2, "AC2: a still two parents after unrelated recycle");
    }

    // ── AC3 second recycle of same slot ────────────────────────────
    {
        std::println("\n--- #2412 AC3: 2nd recycle also clears edges ---");
        FlatAST flat;
        const NodeId root = flat.add_node(NodeTag::Begin);
        flat.root = root;
        NodeId leaf = NULL_NODE;

        for (int round = 1; round <= 2; ++round) {
            leaf = flat.add_node(NodeTag::LiteralInt);
            CHECK(flat.incoming_parent_edge_count_raw(leaf) == 0,
                  "AC3: empty edges after add/reset");
            flat.insert_child(root, 0, leaf);
            flat.ensure_incoming_parent_index();
            CHECK(flat.incoming_parent_edge_count_raw(leaf) == 1, "AC3: edge after attach");
            // Dirty + remove leaves raw edges; recycle then re-add (2nd pass reuses slot).
            flat.mark_incoming_parent_index_dirty();
            flat.remove_child(root, 0);
            CHECK(flat.incoming_parent_edge_count_raw(leaf) == 1,
                  "AC3: stale raw edges before recycle");
            (void)flat.recycle_dead_nodes();
            CHECK(flat.is_free_slot(leaf), "AC3: leaf free after recycle");
        }
        // Final re-add after 2nd free — still empty (the critical 2nd-recycle assert).
        leaf = flat.add_node(NodeTag::LiteralInt);
        CHECK(flat.incoming_parent_edge_count_raw(leaf) == 0, "AC3: 2nd recycle cleared edges");
    }

    // ── AC4 clean-index path still clears (no regression) ──────────
    {
        std::println("\n--- #2412 AC4: clean-index recycle clears ---");
        FlatAST flat;
        const NodeId child = flat.add_node(NodeTag::LiteralInt);
        const NodeId parent = flat.add_node(NodeTag::Begin);
        flat.root = parent;
        flat.insert_child(parent, 0, child);
        flat.ensure_incoming_parent_index();
        flat.remove_child(parent, 0); // clean → incremental edge drop
        CHECK(flat.incoming_parent_edge_count_raw(child) == 0, "AC4: clean remove drops edge");
        (void)flat.recycle_dead_nodes();
        const NodeId reused = flat.add_node(NodeTag::Variable);
        CHECK(flat.incoming_parent_edge_count_raw(reused) == 0, "AC4: clean recycle empty");
        CHECK(!flat.incoming_parent_index_dirty(), "AC4: stayed clean");
        (void)NULL_NODE;
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
