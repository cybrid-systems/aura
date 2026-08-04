// @category: unit
// @reason: Issue #2392 — fixup_deltas documents rebase model and clamps
// overflow / OOB (delta + parent_id) to NULL_NODE instead of corrupt edges.
//
//   AC1: valid deltas → absolute children restored correctly
//   AC2: over-large / overflowing deltas → NULL_NODE, no corrupt state
//   AC3: happy absolute tree + fixup path does not crash (round-trip style)
//   AC4: this test + source-cite + gate
//   AC5: no abort; ASan/TSAN via normal unit path

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.core.ast;

namespace {

using aura::ast::fixup_deltas;
using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// ── AC1: valid parent-relative deltas rebase to absolute ──
static void ac1_valid_deltas() {
    std::println("\n--- #2392 AC1: valid deltas → absolute children ---");
    FlatAST flat;
    // Parent first so children ids > parent (forward deltas, no wrap).
    const NodeId parent = flat.add_node(NodeTag::Begin);
    const NodeId c0 = flat.add_literal(1);
    const NodeId c1 = flat.add_literal(2);
    flat.insert_child(parent, 0, c0);
    flat.insert_child(parent, 1, c1);
    CHECK(flat.children(parent).size() == 2, "AC1: 2 children after insert");
    CHECK(flat.children(parent)[0] == c0 && flat.children(parent)[1] == c1,
          "AC1: absolute children");
    CHECK(c0 > parent && c1 > parent, "AC1: children after parent in id space");

    // Convert absolute → parent-relative deltas (wire encoding model).
    // absolute = delta + parent_id  ⇒  delta = absolute - parent_id
    const NodeId d0 = static_cast<NodeId>(c0 - parent);
    const NodeId d1 = static_cast<NodeId>(c1 - parent);
    flat.set_child(parent, 0, d0);
    flat.set_child(parent, 1, d1);
    CHECK(flat.children(parent)[0] == d0, "AC1: planted delta0");
    CHECK(flat.children(parent)[1] == d1, "AC1: planted delta1");

    fixup_deltas(flat);

    CHECK(flat.children(parent).size() == 2, "AC1: size preserved after fixup");
    CHECK(flat.children(parent)[0] == c0, "AC1: rebased child0 == absolute c0");
    CHECK(flat.children(parent)[1] == c1, "AC1: rebased child1 == absolute c1");
    std::println("  parent={} c0={} c1={} after fixup: [{}, {}]", parent, c0, c1,
                 flat.children(parent)[0], flat.children(parent)[1]);
}

// ── AC2: over-large delta clamps to NULL_NODE ──
static void ac2_oob_and_overflow() {
    std::println("\n--- #2392 AC2: OOB / overflow deltas → NULL_NODE ---");
    FlatAST flat;
    const NodeId parent = flat.add_node(NodeTag::Begin);
    const NodeId c0 = flat.add_literal(1);
    flat.insert_child(parent, 0, c0);
    // Plant an over-large delta (rebased would be parent + huge >= size).
    const NodeId huge_delta = static_cast<NodeId>(flat.size() + 100);
    flat.set_child(parent, 0, huge_delta);
    CHECK(flat.children(parent)[0] == huge_delta, "AC2: planted huge delta");

    fixup_deltas(flat);

    CHECK(flat.children(parent)[0] == NULL_NODE, "AC2: over-large delta clamped to NULL_NODE");
    // Tree size unchanged; no ghost absolute id past size().
    CHECK(flat.size() >= 2, "AC2: size stable");

    // Overflow path: delta chosen so delta + parent wraps uint32.
    {
        FlatAST f3;
        // Need parent id >= 2 so (max - p + 1) != NULL_NODE (=max).
        (void)f3.add_literal(0);
        (void)f3.add_literal(1);
        const NodeId p = f3.add_node(NodeTag::Begin);
        CHECK(p >= 2, "AC2: parent id >= 2 for wrap case");
        const NodeId leaf = f3.add_literal(7);
        f3.insert_child(p, 0, leaf);
        // max - p + 1 + p == max + 1 → wraps; delta itself is max-p+1 ≠ max.
        const NodeId delta_ov = static_cast<NodeId>(std::numeric_limits<NodeId>::max() - p + 1);
        CHECK(delta_ov != NULL_NODE, "AC2: overflow delta not the NULL sentinel");
        f3.set_child(p, 0, delta_ov);
        fixup_deltas(f3);
        CHECK(f3.children(p)[0] == NULL_NODE, "AC2: overflow clamp to NULL_NODE");
        std::println("  overflow: p={} delta={} → child={}", p, delta_ov, f3.children(p)[0]);
    }
}

// ── AC3: absolute tree already correct — fixup with parent=0 is stable ──
static void ac3_parent_zero_absolute() {
    std::println("\n--- #2392 AC3: parent id 0 absolute children round-trip ---");
    FlatAST flat;
    // First node is parent id 0 when FlatAST starts empty.
    const NodeId p = flat.add_node(NodeTag::Begin);
    CHECK(p == 0, "AC3: first node id 0");
    const NodeId a = flat.add_literal(1);
    const NodeId b = flat.add_literal(2);
    flat.insert_child(p, 0, a);
    flat.insert_child(p, 1, b);
    // For parent 0, absolute == delta. Plant same values as deltas.
    flat.set_child(p, 0, a); // a - 0 == a
    flat.set_child(p, 1, b);
    fixup_deltas(flat);
    CHECK(flat.children(p)[0] == a && flat.children(p)[1] == b,
          "AC3: parent-0 absolute preserved through fixup");
}

// ── AC4: source-cite + gate ──
static void ac4_source_and_gate() {
    std::println("\n--- #2392 AC4: source-cite + gate ---");
    const auto impl = read_file("src/core/ast_impl.cpp");
    const auto ixx = read_file("src/core/ast.ixx");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto linter = read_file("scripts/coverage/checks/check_fixup_deltas_2392.py");

    CHECK(impl.find("Issue #2392") != std::string::npos, "AC4: cites #2392");
    CHECK(impl.find("fixup_deltas") != std::string::npos, "AC4: fixup_deltas present");
    CHECK(impl.find("NULL_NODE") != std::string::npos, "AC4: clamps to NULL_NODE");
    CHECK(impl.find("relative") != std::string::npos || impl.find("delta") != std::string::npos,
          "AC4: documents delta / relative model");
    // No bare unchecked `cid + id` as sole set_child arg without bounds.
    CHECK(impl.find("rebased") != std::string::npos || impl.find("cand") != std::string::npos,
          "AC4: checked rebased/cand path");
    CHECK(ixx.find("2392") != std::string::npos, "AC4: export declaration cites #2392");
    CHECK(cmake.find("test_fixup_deltas_2392") != std::string::npos, "AC4: CMake");
    CHECK(build.find("check_fixup_deltas_2392") != std::string::npos ||
              build.find("cmd_fixup_deltas_coverage") != std::string::npos,
          "AC4: build.py gate");
    CHECK(!linter.empty(), "AC4: coverage linter present");
}

} // namespace

int main() {
    std::println("=== Issue #2392: fixup_deltas safe rebase ===");
    ac1_valid_deltas();
    ac2_oob_and_overflow();
    ac3_parent_zero_absolute();
    ac4_source_and_gate();
    std::println("\n=== #2392 results: passed={} failed={} ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
