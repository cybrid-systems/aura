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
#include <vector>

import std;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::MutationStatus;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::test::g_failed;
using aura::test::g_passed;

// Issue #5312: recycle must not walk released rebind history, and a
// snapshot must not pin the contiguous mutation log (the next append
// would deep-copy it, and every retained image would keep a copy).
void test_rebind_rollback_scan_5312() {
    std::println("\n--- #5312: rebind rollback scan stays on live pins ---");
    FlatAST flat;
    const NodeId keep = flat.add_node(NodeTag::LiteralInt);
    const NodeId root = flat.add_node(NodeTag::Begin);
    flat.root = root;
    flat.insert_child(root, 0, keep);
    const NodeId dead_child = flat.add_node(NodeTag::LiteralInt);
    const NodeId dead_body = flat.add_node(NodeTag::Begin);
    flat.insert_child(dead_body, 0, dead_child);

    auto pin = [&](NodeId body) {
        flat.add_mutation_with_rollback(root, "rebind", "n", "n", "s", MutationStatus::Committed,
                                        /*field_offset=*/0, static_cast<std::uint64_t>(body),
                                        static_cast<std::uint64_t>(body), /*has_rollback=*/true);
    };
    pin(keep);
    for (int i = 0; i < 200; ++i)
        pin(dead_body);
    // One non-rebind record must not enter the scan set.
    flat.add_mutation(root, "annotate", "a", "b", "s");
    const auto log_before = flat.mutation_log_size();
    CHECK(flat.live_rebind_rollback_count() == 201, "5312: every rebind pin is indexed");

    const auto visits_before = flat.rebind_rollback_scan_visits();
    (void)flat.recycle_dead_nodes();
    const auto visited = flat.rebind_rollback_scan_visits() - visits_before;
    CHECK(visited == 201, "5312: first recycle visits the pins, not the whole log");
    CHECK(flat.mutation_log_size() == log_before, "5312: recycle keeps the audit log");
    CHECK(flat.live_rebind_rollback_count() == 1, "5312: released rebind pins leave the index");
    CHECK(!flat.is_free_slot(keep), "5312: a whole rollback body stays pinned (#1408)");
    CHECK(flat.is_free_slot(dead_body), "5312: a broken rollback body is freed");
    // insert_child logs structural-insert-child (has_rollback, not a rebind
    // pin) before the rebind records. Those flags stay: rollback restores
    // them through the children snapshot (#222), not through this scan.
    const auto& log = std::as_const(flat).all_mutations();
    std::size_t structural_flags = 0;
    std::size_t kept_flags = 0;
    std::size_t released_cleared = 0;
    for (const auto& rec : log) {
        if (rec.operator_name == "structural-insert-child") {
            if (rec.has_rollback_data)
                ++structural_flags;
        } else if (rec.operator_name == "rebind") {
            if (rec.old_value == static_cast<std::uint64_t>(keep)) {
                if (rec.has_rollback_data)
                    ++kept_flags;
            } else if (!rec.has_rollback_data) {
                ++released_cleared;
            }
        }
    }
    CHECK(structural_flags == 2, "5312: structural records keep rollback flags");
    CHECK(kept_flags == 1, "5312: live pin keeps rollback data");
    CHECK(released_cleared == 200, "5312: released record drops rollback data");

    const auto visits_mid = flat.rebind_rollback_scan_visits();
    (void)flat.recycle_dead_nodes();
    CHECK(flat.rebind_rollback_scan_visits() - visits_mid == 1,
          "5312: later recycle does not walk released history");
    CHECK(!flat.is_free_slot(keep), "5312: second recycle still pins the live body");

    std::println("\n--- #5312: snapshot does not pin the mutation log ---");
    FlatAST logged;
    const NodeId n = logged.add_node(NodeTag::LiteralInt);
    logged.root = n;
    std::uint64_t first_id = 0;
    for (int i = 0; i < 80; ++i) {
        const auto id = logged.add_mutation(n, "annotate", "a", "b", "s");
        if (i == 0)
            first_id = id;
    }
    const auto base = logged.capture_image();
    const auto det0 = logged.mutation_log_detach_count();
    std::vector<std::shared_ptr<const FlatAST::Image>> imgs;
    imgs.reserve(30);
    for (int i = 0; i < 30; ++i) {
        logged.add_mutation(n, "annotate", "a", "b", "s");
        imgs.push_back(logged.capture_image());
    }
    CHECK(logged.mutation_log_detach_count() == det0,
          "5312: append after capture does not deep-copy the log");
    CHECK(logged.mutation_log_size() == 110, "5312: live log grew across captures");
    logged.assign_from_image(base);
    CHECK(logged.mutation_log_size() == 80, "5312: restore brings back the snapshot log");
    CHECK(std::as_const(logged).all_mutations().front().mutation_id == first_id,
          "5312: restored records keep their ids");
    logged.assign_from_image(imgs.back());
    CHECK(logged.mutation_log_size() == 110, "5312: a later image restores its own tail");
    const auto tree = FlatAST::materialize(base);
    CHECK(tree && tree->mutation_log_size() == 0,
          "5312: lazy source materialize does not rebuild the audit log");
    CHECK(tree && tree->root == n, "5312: lazy source materialize keeps the tree");
}

} // namespace

int run_test_reset_slot_parent_edges() {
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

    test_rebind_rollback_scan_5312();

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_reset_slot_parent_edges();
}
#endif
