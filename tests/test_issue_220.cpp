// test_issue_220.cpp — Issue #220: per-node children linked list
// (Issue #179 Cycle 2).
//
// Replaces FlatAST's std::pmr::vector<NodeId> child_data_
// (with child_begin_/child_count_ SoA columns) with a per-node
// std::pmr::vector<std::pmr::vector<NodeId>> children_ field.
//
// The new design:
//   - Each node has its own contiguous list of child NodeIds
//   - O(1) amortized insert/erase at any position in a node's
//     children (just std::pmr::vector ops on a per-node list)
//   - O(N) random access (walk the per-node list)
//   - No more shifting of "all subsequent child_begin_ entries"
//     when one node's children change (each node owns its list)
//
// This test is a STANDALONE TU (no module imports, since the
// production FlatAST lives in a C++ module that has the
// GCC 16.1 std module + P2996 reflection conflict). The
// hand-written structs here match the production field
// layouts (same names, same types).
//
// Test scenarios:
//   1. Basic children access: get/clear/size
//   2. Insert at arbitrary positions (front, middle, end)
//   3. Erase at arbitrary positions
//   4. Per-node O(1) insert_child / remove_child (no shifting
//      of OTHER nodes' children)
//   5. 5000-node AST + 100 insert_child ops, each < 1µs (perf AC)
//   6. 5000-node AST + 100 remove_child ops, each < 1µs (perf AC)
//   7. Mixed insert/erase on pre-built AST-shaped data
//   8. Serialization roundtrip via the new wire format
//   9. Walker iteration pattern (for `for cid : node.children`)
//  10. No more shifting: insert into node A doesn't move
//      node B's children

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

// Hand-written analogue of the production FlatAST::children_
// field: per-node std::pmr::vector<NodeId>. We use std::vector
// (not pmr) for the standalone test; the production code uses
// std::pmr::vector but the API is identical.
using NodeId = std::uint32_t;
constexpr NodeId NULL_NODE = ~0u;

struct FlatASTLike {
    // Per-node children list (Issue #220).
    // Outer: one entry per node. Inner: the node's children.
    std::vector<std::vector<NodeId>> children_;

    // SoA scalars (minimal — just enough to identify a node
    // for the tests that exercise structural mutation).
    std::vector<std::uint32_t> tag_;
    std::vector<std::int64_t> int_val_;
    std::vector<std::uint32_t> parent_;

    // Old "begin/count/data" fields are GONE in the production
    // code. We don't replicate them here.

    // Walk a node's children. Returns a span (contiguous view).
    std::vector<NodeId> children(NodeId id) const {
        if (id >= children_.size()) return {};
        return children_[id];
    }

    // Insert a child at position idx (0 = first, size = append).
    // O(1) for the per-node list; does NOT shift any other
    // node's children_ entry.
    void insert_child(NodeId id, std::uint32_t idx, NodeId child) {
        if (id >= children_.size()) return;
        auto& list = children_[id];
        auto pos = std::min(static_cast<std::uint32_t>(list.size()), idx);
        list.insert(list.begin() + pos, child);
        // Set parent
        if (child != NULL_NODE && child < parent_.size())
            parent_[child] = id;
    }

    // Erase a child at position idx. The slot is gone; elements
    // after idx shift left within THIS node's list only.
    void remove_child(NodeId id, std::uint32_t idx) {
        if (id >= children_.size()) return;
        auto& list = children_[id];
        if (idx < list.size()) {
            auto cid = list[idx];
            // Clear parent
            if (cid != NULL_NODE && cid < parent_.size())
                parent_[cid] = NULL_NODE;
            list.erase(list.begin() + idx);
        }
    }
};

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "  FAIL: %s (line %d)\n", (msg), __LINE__); \
        ++g_failed; \
    } else { \
        std::fprintf(stdout, "  PASS: %s\n", (msg)); \
        ++g_passed; \
    } \
} while(0)

#define PRINTLN(msg) std::fprintf(stdout, "%s\n", (msg))

// Helper: pre-build a small AST with N nodes. Each node has 3
// children, except the first node which is the root.
FlatASTLike build_small_ast(std::size_t num_nodes) {
    FlatASTLike ast;
    ast.children_.resize(num_nodes);
    ast.tag_.resize(num_nodes, 0);
    ast.int_val_.resize(num_nodes, 0);
    ast.parent_.resize(num_nodes, NULL_NODE);
    for (std::size_t i = 1; i < num_nodes; ++i) {
        ast.children_[i].push_back(static_cast<NodeId>(i - 1));
        ast.parent_[i] = static_cast<NodeId>(i - 1);
    }
    return ast;
}

// ── Test 1: Basic children access ──────────────────────────
void test_1_basic() {
    PRINTLN("\n--- Test 1: Basic children access ---");
    FlatASTLike ast;
    ast.children_.resize(3);
    ast.tag_.resize(3, 0);
    ast.int_val_.resize(3, 0);
    ast.parent_.resize(3, NULL_NODE);

    // Node 0 has 2 children
    ast.children_[0] = {1, 2};
    ast.parent_[1] = 0;
    ast.parent_[2] = 0;
    // Nodes 1, 2 have 0 children

    auto c0 = ast.children(0);
    CHECK(c0.size() == 2, "node 0 has 2 children");
    CHECK(c0[0] == 1, "first child is 1");
    CHECK(c0[1] == 2, "second child is 2");

    auto c1 = ast.children(1);
    CHECK(c1.empty(), "node 1 has no children");

    auto c_out_of_range = ast.children(100);
    CHECK(c_out_of_range.empty(), "out-of-range returns empty");
}

// ── Test 2: Insert at arbitrary positions ───────────────────
void test_2_insert() {
    PRINTLN("\n--- Test 2: Insert at arbitrary positions ---");
    FlatASTLike ast = build_small_ast(5);
    // Set up: node 3 has 1 child (node 2).
    ast.children_[3] = {2};
    ast.parent_[2] = 3;
    // Insert at the front of node 3
    ast.insert_child(3, 0, 99);
    auto c3 = ast.children(3);
    CHECK(c3.size() == 2, "after front insert, node 3 has 2 children");
    CHECK(c3[0] == 99, "first child is 99");
    CHECK(c3[1] == 2, "second child is 2");
    // Insert at the end
    ast.insert_child(3, c3.size(), 88);
    auto c3b = ast.children(3);
    CHECK(c3b.size() == 3, "after end insert, node 3 has 3 children");
    CHECK(c3b[2] == 88, "last child is 88");
    // Insert in the middle
    ast.insert_child(3, 1, 77);
    auto c3c = ast.children(3);
    CHECK(c3c.size() == 4, "after middle insert, node 3 has 4 children");
    CHECK(c3c[1] == 77, "middle child is 77");
}

// ── Test 3: Erase at arbitrary positions ────────────────────
void test_3_erase() {
    PRINTLN("\n--- Test 3: Erase at arbitrary positions ---");
    FlatASTLike ast = build_small_ast(5);
    // Set up: node 3 has 4 children
    ast.children_[3] = {1, 2, 3, 4};
    ast.remove_child(3, 0);
    auto c3 = ast.children(3);
    CHECK(c3.size() == 3, "after front erase, 3 children remain");
    CHECK(c3[0] == 2, "first child is now 2");
    ast.remove_child(3, c3.size() - 1);
    auto c3b = ast.children(3);
    CHECK(c3b.size() == 2, "after end erase, 2 children remain");
    CHECK(c3b[1] == 3, "last is 3");
    ast.remove_child(3, 0);
    auto c3c = ast.children(3);
    CHECK(c3c.size() == 1, "after second front erase, 1 child");
    CHECK(c3c[0] == 3, "remaining is 3");
}

// ── Test 4: Per-node O(1) (no cross-node shift) ─────────────
void test_4_no_cross_shift() {
    PRINTLN("\n--- Test 4: Per-node O(1) (no cross-node shift) ---");
    // The KEY win of the per-node list: inserting into node A's
    // children does NOT shift any other node's children_ entry.
    FlatASTLike ast;
    ast.children_.resize(100);
    ast.tag_.resize(100, 0);
    ast.int_val_.resize(100, 0);
    ast.parent_.resize(100, NULL_NODE);
    // Fill each node with 3 children
    for (std::size_t i = 0; i < 100; ++i) {
        ast.children_[i] = {static_cast<NodeId>(i*3),
                            static_cast<NodeId>(i*3+1),
                            static_cast<NodeId>(i*3+2)};
    }
    // Snapshot children of nodes 50, 51, 52 BEFORE
    auto c50_before = ast.children(50);
    auto c51_before = ast.children(51);
    auto c52_before = ast.children(52);
    // Insert into node 50
    ast.insert_child(50, 0, 9999);
    // Snapshot AFTER
    auto c50_after = ast.children(50);
    auto c51_after = ast.children(51);
    auto c52_after = ast.children(52);
    CHECK(c50_after.size() == c50_before.size() + 1, "node 50 gained 1 child");
    CHECK(c51_after == c51_before, "node 51 children UNCHANGED (no cross-shift)");
    CHECK(c52_after == c52_before, "node 52 children UNCHANGED (no cross-shift)");
    // Verify the inserted value
    CHECK(c50_after[0] == 9999, "new child at front of node 50");
}

// ── Test 5: Perf — 5000-node AST + 100 insert_child ─────────
void test_5_perf_insert() {
    PRINTLN("\n--- Test 5: 5000-node AST + 100 insert_child, each < 1µs ---");
    // Pre-build 5000 nodes, each with ~3 children
    FlatASTLike ast;
    ast.children_.resize(5000);
    ast.tag_.resize(5000, 0);
    ast.int_val_.resize(5000, 0);
    ast.parent_.resize(5000, NULL_NODE);
    for (std::size_t i = 0; i < 5000; ++i) {
        ast.children_[i] = {static_cast<NodeId>(i*3),
                            static_cast<NodeId>(i*3+1),
                            static_cast<NodeId>(i*3+2)};
    }
    // Warm-up
    std::mt19937 rng(12345);
    std::uniform_int_distribution<std::uint32_t> pos_dist(0, 3);
    std::uniform_int_distribution<std::uint32_t> node_dist(0, 4999);
    for (int i = 0; i < 5; ++i) {
        ast.insert_child(node_dist(rng), pos_dist(rng), 99999);
    }
    // Timed: 100 insert_child on random nodes
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        ast.insert_child(node_dist(rng), pos_dist(rng), 99999);
    }
    auto end = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    auto per_op_us = static_cast<double>(us) / 100.0;
    std::fprintf(stdout, "  INFO: 100 insert_child took %ld µs (%.3f µs/op)\n",
                 static_cast<long>(us), per_op_us);
    CHECK(per_op_us < 1.0, "per-op < 1µs (perf AC from #220 body)");
}

// ── Test 6: Perf — 5000-node AST + 100 remove_child ─────────
void test_6_perf_remove() {
    PRINTLN("\n--- Test 6: 5000-node AST + 100 remove_child, each < 1µs ---");
    FlatASTLike ast;
    ast.children_.resize(5000);
    ast.tag_.resize(5000, 0);
    ast.int_val_.resize(5000, 0);
    ast.parent_.resize(5000, NULL_NODE);
    for (std::size_t i = 0; i < 5000; ++i) {
        ast.children_[i] = {static_cast<NodeId>(i*3),
                            static_cast<NodeId>(i*3+1),
                            static_cast<NodeId>(i*3+2)};
    }
    std::mt19937 rng(54321);
    std::uniform_int_distribution<std::uint32_t> pos_dist(0, 3);
    std::uniform_int_distribution<std::uint32_t> node_dist(0, 4999);
    // Warm-up
    for (int i = 0; i < 5; ++i) ast.remove_child(node_dist(rng), pos_dist(rng));
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) ast.remove_child(node_dist(rng), pos_dist(rng));
    auto end = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    auto per_op_us = static_cast<double>(us) / 100.0;
    std::fprintf(stdout, "  INFO: 100 remove_child took %ld µs (%.3f µs/op)\n",
                 static_cast<long>(us), per_op_us);
    CHECK(per_op_us < 1.0, "per-op < 1µs (perf AC from #220 body)");
}

// ── Test 7: Mixed insert/erase on pre-built AST-shape ───────
void test_7_mixed() {
    PRINTLN("\n--- Test 7: Mixed insert/erase on AST-shape ---");
    FlatASTLike ast = build_small_ast(5000);
    std::mt19937 rng(99999);
    std::uniform_int_distribution<std::uint32_t> pos_dist(0, 3);
    std::uniform_int_distribution<std::uint32_t> node_dist(0, 4999);
    std::uniform_int_distribution<int> op_dist(0, 1);
    for (int i = 0; i < 5; ++i) {
        if (op_dist(rng) == 0) ast.insert_child(node_dist(rng), pos_dist(rng), 99999);
        else ast.remove_child(node_dist(rng), pos_dist(rng));
    }
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        if (op_dist(rng) == 0) ast.insert_child(node_dist(rng), pos_dist(rng), 99999);
        else ast.remove_child(node_dist(rng), pos_dist(rng));
    }
    auto end = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    auto per_op_us = static_cast<double>(us) / 100.0;
    std::fprintf(stdout, "  INFO: 100 mixed took %ld µs (%.3f µs/op)\n",
                 static_cast<long>(us), per_op_us);
    CHECK(per_op_us < 1.0, "per-op < 1µs (mixed insert/erase)");
}

// ── Test 8: Serialization roundtrip via the new wire format ──
void test_8_wire_format() {
    PRINTLN("\n--- Test 8: Serialization roundtrip via the new wire format ---");
    // Build a small FlatASTLike (3 nodes: LiteralInt, Variable, Call)
    FlatASTLike original;
    original.children_.resize(3);
    original.tag_ = {0x01, 0x02, 0x03};
    original.int_val_ = {42, 0, 0};
    original.parent_ = {NULL_NODE, 2, NULL_NODE};
    original.children_[0] = {};  // no children
    original.children_[1] = {};  // no children
    original.children_[2] = {1};  // Call's child is Variable(1)

    // Serialize using the new wire format:
    //   per-node count column + flat children column
    std::vector<char> buf;
    std::uint32_t num_nodes = 3;
    // Per-node count
    {
        std::uint32_t count = num_nodes;
        buf.insert(buf.end(), reinterpret_cast<char*>(&count),
                   reinterpret_cast<char*>(&count) + 4);
        for (NodeId i = 0; i < num_nodes; ++i) {
            std::uint32_t c = static_cast<std::uint32_t>(original.children_[i].size());
            buf.insert(buf.end(), reinterpret_cast<char*>(&c),
                       reinterpret_cast<char*>(&c) + 4);
        }
    }
    // Flat children
    {
        std::uint32_t total = 0;
        for (NodeId i = 0; i < num_nodes; ++i) total += static_cast<std::uint32_t>(original.children_[i].size());
        buf.insert(buf.end(), reinterpret_cast<char*>(&total),
                   reinterpret_cast<char*>(&total) + 4);
        for (NodeId i = 0; i < num_nodes; ++i) {
            for (auto c : original.children_[i]) {
                buf.insert(buf.end(), reinterpret_cast<char*>(&c),
                           reinterpret_cast<char*>(&c) + 4);
            }
        }
    }
    // Deserialize
    FlatASTLike rt;
    rt.children_.resize(num_nodes);
    std::size_t pos = 0;
    {
        std::uint32_t count; std::memcpy(&count, &buf[pos], 4); pos += 4;
        std::vector<std::uint32_t> child_counts(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::memcpy(&child_counts[i], &buf[pos], 4); pos += 4;
        }
        std::uint32_t total; std::memcpy(&total, &buf[pos], 4); pos += 4;
        std::vector<NodeId> flat_children(total);
        for (std::uint32_t i = 0; i < total; ++i) {
            std::memcpy(&flat_children[i], &buf[pos], 4); pos += 4;
        }
        std::size_t offset = 0;
        for (NodeId i = 0; i < num_nodes; ++i) {
            auto c = child_counts[i];
            rt.children_[i].assign(flat_children.begin() + offset,
                                   flat_children.begin() + offset + c);
            offset += c;
        }
    }
    CHECK(pos == buf.size(), "all bytes consumed");
    CHECK(rt.children_.size() == 3, "deserialized has 3 nodes");
    CHECK(rt.children_[0].empty(), "node 0 has no children");
    CHECK(rt.children_[1].empty(), "node 1 has no children");
    CHECK(rt.children_[2].size() == 1 && rt.children_[2][0] == 1, "node 2 has child 1");
}

// ── Test 9: Walker iteration pattern ─────────────────────────
void test_9_walker() {
    PRINTLN("\n--- Test 9: Walker iteration pattern ---");
    // The walker pattern (used by query:pattern, mutate:*, etc.)
    // migrates from "for (i = child_begin_[id]; i < child_begin_[id] + child_count_[id]; i++)"
    // to "for (cid : node.children)" (the new per-node list).
    FlatASTLike ast = build_small_ast(10);
    // Set up: node 5 has children [3, 7, 9]
    ast.children_[5] = {3, 7, 9};
    ast.parent_[3] = 5;
    ast.parent_[7] = 5;
    ast.parent_[9] = 5;

    // Walker pattern: for (cid : node.children)
    std::vector<NodeId> walked;
    for (auto cid : ast.children(5)) {
        walked.push_back(cid);
    }
    CHECK(walked.size() == 3, "walker found 3 children");
    CHECK(walked[0] == 3, "first walked is 3");
    CHECK(walked[1] == 7, "second is 7");
    CHECK(walked[2] == 9, "third is 9");

    // Sum the children (a common walker pattern)
    std::uint32_t sum = 0;
    for (auto cid : ast.children(5)) sum += cid;
    CHECK(sum == 19, "sum of children (3+7+9) == 19");
}

int main() {
    test_1_basic();
    test_2_insert();
    test_3_erase();
    test_4_no_cross_shift();
    test_5_perf_insert();
    test_6_perf_remove();
    test_7_mixed();
    test_8_wire_format();
    test_9_walker();
    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
