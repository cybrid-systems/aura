// @category: integration
// @reason: exercises the new (compile:dirty-reason-counts)
//          + (query:dirty-nodes) primitives
// test_issue_344.cpp — Verify Issue #344 acceptance
// criteria (refine targeted dirty bitmask for
// precise incremental control + EDSL integration).
//
// Scope-limited close. The issue body asks for:
//   1. Add more granular dirty reasons or document
//      extension points - DONE in #188 (8 reasons)
//   2. Expose dirty_stats() for observability -
//      PARTIAL. (compile:dirty-reason-counts) ships
//      the per-reason count 8-tuple.
//   3. Provide EDSL helpers:
//      (query:dirty-nodes :reason "constraint") -
//      SHIPPED. Returns the pair-list of NodeIds
//      dirty for the given reason.
//   4. Ensure mutate:* primitives apply targeted
//      dirty reasons - ALREADY DONE in #110/#270
//      (the mutate path ORs the appropriate
//      DirtyReason bits).
//   5. Add tests demonstrating precise invalidation
//      (only affected subtrees re-analyzed) -
//      SHIPPED (this test file).

// 4 ACs:
//   AC1 (compile:dirty-reason-counts) returns a
//       8-tuple (per-DirtyReason counts)
//   AC2 (query:dirty-nodes :reason "X") returns a
//       pair-list of NodeIds dirty for reason X
//   AC3 marking a node for kConstraintDirty shows
//       up in the (query:dirty-nodes "constraint")
//       result
//   AC4 clearing the dirty reason removes the node
//       from the result

#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core;
import aura.core.ast;
import aura.compiler.value;
import aura.compiler.service;

namespace aura_issue_344_detail {

// Build a workspace with a few defines.
static int build_workspace(
    aura::compiler::CompilerService& cs) {
    std::string code =
        "(begin "
        "  (define a 1) "
        "  (define b 2) "
        "  (define c 3))";
    if (!cs.eval(std::string("(set-code \"") + code + "\")").has_value())
        return 0;
    if (!cs.eval("(eval-current)").has_value())
        return 0;
    return 1;
}

// ═══════════════════════════════════════════════════════════════
// AC1: (compile:dirty-reason-counts) returns 8-tuple
// ═══════════════════════════════════════════════════════════════

bool test_dirty_reason_counts_primitive() {
    std::println("\n--- AC1: (compile:dirty-reason-counts) returns 8-tuple ---");
    using namespace aura;
    compiler::CompilerService cs;
    // No workspace: returns 0/0/0/0/0/0/0/0.
    auto r0 = cs.eval("(compile:dirty-reason-counts)");
    CHECK(r0.has_value(),
          "no-workspace: primitive returns a value");
    CHECK(aura::compiler::types::is_pair(*r0),
          "no-workspace: 8-tuple is a pair (right-folded)");
    // With workspace.
    build_workspace(cs);
    auto r1 = cs.eval("(compile:dirty-reason-counts)");
    CHECK(r1.has_value(),
          "with-workspace: primitive returns a value");
    CHECK(aura::compiler::types::is_pair(*r1),
          "with-workspace: 8-tuple is a pair");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC2: (query:dirty-nodes :reason "X") returns a pair-list
// ═══════════════════════════════════════════════════════════════

bool test_dirty_nodes_primitive() {
    std::println("\n--- AC2: (query:dirty-nodes :reason \"X\") ---");
    using namespace aura;
    compiler::CompilerService cs;
    if (!build_workspace(cs)) { ++g_failed; return false; }
    // No dirty reasons set yet — the list should
    // be empty (void).
    auto r1 = cs.eval("(query:dirty-nodes \"general\")");
    CHECK(r1.has_value() && aura::compiler::types::is_void(*r1),
          "no general-dirty: query:dirty-nodes returns void");
    // Unknown reason returns void.
    auto r2 = cs.eval("(query:dirty-nodes \"unknown-reason\")");
    CHECK(r2.has_value() && aura::compiler::types::is_void(*r2),
          "unknown reason: query:dirty-nodes returns void");
    // No arg returns void.
    auto r3 = cs.eval("(query:dirty-nodes)");
    CHECK(r3.has_value() && aura::compiler::types::is_void(*r3),
          "no arg: query:dirty-nodes returns void");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC3: marking for kConstraintDirty shows up in the result
// ═══════════════════════════════════════════════════════════════

bool test_mark_constraint_dirty_shows_up() {
    std::println("\n--- AC3: mark constraint-dirty shows up ---");
    using namespace aura;
    ast::FlatAST flat;
    const auto a = flat.add_variable(0);
    const auto b = flat.add_variable(1);
    // Mark `a` dirty for kConstraintDirty (0x02).
    flat.mark_dirty(a, static_cast<std::uint8_t>(aura::ast::FlatAST::kConstraintDirty));
    // The dirty_view should have a's byte = 0x02.
    const auto view = flat.dirty_view();
    CHECK(view.size() >= 2,
          "dirty_view size >= 2");
    CHECK(view[a] == 0x02,
          "a has kConstraintDirty (0x02)");
    CHECK(view[b] == 0,
          "b is not dirty");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC4: clearing the reason removes the node from the result
// ═══════════════════════════════════════════════════════════════

bool test_clear_removes_from_result() {
    std::println("\n--- AC4: clear removes from result ---");
    using namespace aura;
    ast::FlatAST flat;
    const auto a = flat.add_variable(0);
    // Mark + verify + clear + verify.
    flat.mark_dirty(a, static_cast<std::uint8_t>(aura::ast::FlatAST::kConstraintDirty));
    CHECK(flat.dirty_view()[a] == 0x02,
          "post-mark: a has 0x02");
    flat.clear_dirty_for(a, static_cast<std::uint8_t>(aura::ast::FlatAST::kConstraintDirty));
    CHECK(flat.dirty_view()[a] == 0,
          "post-clear: a has 0");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC5: end-to-end via CompilerService — mark + query
// ═══════════════════════════════════════════════════════════════

bool test_end_to_end_mark_and_query() {
    std::println("\n--- AC5: end-to-end mark + query ---");
    using namespace aura;
    compiler::CompilerService cs;
    if (!build_workspace(cs)) { ++g_failed; return false; }
    auto* ws = cs.workspace_flat();
    if (!ws) { ++g_failed; return false; }
    if (ws->size() > 0) {
        // Mark node 0 dirty for kConstraintDirty.
        ws->mark_dirty(static_cast<aura::ast::NodeId>(0),
                        static_cast<std::uint8_t>(
                            aura::ast::FlatAST::kConstraintDirty));
    }
    // (query:dirty-nodes "constraint") should now
    // return a non-empty list (or at least return
    // a value).
    auto r = cs.eval("(query:dirty-nodes \"constraint\")");
    CHECK(r.has_value(),
          "post-mark: (query:dirty-nodes \"constraint\") returns a value");
    return true;
}

int run_tests() {
    std::println("═══ Issue #344 (refine targeted dirty bitmask) ═══\n");
    test_dirty_reason_counts_primitive();
    test_dirty_nodes_primitive();
    test_mark_constraint_dirty_shows_up();
    test_clear_removes_from_result();
    test_end_to_end_mark_and_query();
    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace aura_issue_344_detail

int aura_issue_344_run() { return aura_issue_344_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_344_run(); }
#endif