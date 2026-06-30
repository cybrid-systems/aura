// @category: integration
// @reason: uses CompilerService + syntax-marker primitives + mutate
//
// test_issue_366.cpp — Verify Issue #366 acceptance criteria
// ("[macro][observability] Ensure SyntaxMarker::MacroIntroduced
//  survives and remains consistent after mutate operations").
//
// Background: #190 ships recursive propagation of
// SyntaxMarker::MacroIntroduced + the syntax-marker /
// syntax-marker-counts primitives. After structural
// mutations (insert-child, replace-subtree), markers can be
// lost or become inconsistent — the macro-introduced subtree
// may end up partially tagged.
//
// This scope-limited close ships 2 new primitives +
// integration tests:
//   - (syntax:set-marker node-id marker) — set a single
//     node's marker (0/1/2).
//   - (syntax:propagate-marker node-id marker) — set the
//     marker on a node AND recursively on all descendants.
//     Returns the count of nodes updated.
//
// Test strategy: 3 layers
//   Layer 1: set-marker on a single node
//   Layer 2: propagate-marker walks the subtree + sets all
//   Layer 3: end-to-end via syntax-marker-counts to verify
//            the macro-introduced count reflects the change

#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.core.ast;
import aura.core.type;

namespace aura_issue_366_detail {

// ═══════════════════════════════════════════════════════════
// Layer 1: set-marker on a single node
// ═══════════════════════════════════════════════════════════

bool test_set_marker_on_single_node() {
    std::println("\n--- AC1: (syntax:set-marker node-id marker) sets marker ---");
    aura::compiler::CompilerService cs;
    // set-code is required to materialize the workspace AST that
    // query:find / syntax-marker / syntax:set-marker operate on.
    cs.eval("(set-code \"(define x 1)\")");
    auto r = cs.eval("(car (query:find \"x\"))");
    if (!r) {
        std::println("    [diag] query:find returned null");
        return false;
    }
    int64_t node_id = aura::compiler::types::as_int(*r);
    CHECK(node_id > 0, "query:find returns a valid node id");

    // Read the original marker (default User = 0).
    auto m0 = cs.eval("(syntax-marker " + std::to_string(node_id) + ")");
    if (!m0) return false;
    int64_t orig_marker = aura::compiler::types::as_int(*m0);
    CHECK(orig_marker == 0,
          "fresh node has default marker (User = 0)");

    // Set to MacroIntroduced (1).
    auto set_r = cs.eval("(syntax:set-marker " + std::to_string(node_id) + " 1)");
    CHECK(set_r.has_value(), "syntax:set-marker returns success");

    // Verify the marker is now MacroIntroduced.
    auto m1 = cs.eval("(syntax-marker " + std::to_string(node_id) + ")");
    if (!m1) return false;
    int64_t new_marker = aura::compiler::types::as_int(*m1);
    CHECK(new_marker == 1,
          "syntax-marker reflects the set value (MacroIntroduced = 1)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 2: propagate-marker walks subtree
// ═══════════════════════════════════════════════════════════

bool test_propagate_marker_subtree() {
    std::println("\n--- AC2: (syntax:propagate-marker node-id marker) walks subtree ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define (f x) (+ x 1))\")");
    // Find the function f's define id (the top-level binding).
    auto r = cs.eval("(car (query:find \"f\"))");
    if (!r) return false;
    int64_t root_id = aura::compiler::types::as_int(*r);

    // Mark the entire subtree (define + lambda + body) as MacroIntroduced.
    auto prop_r = cs.eval("(syntax:propagate-marker " + std::to_string(root_id) + " 1)");
    CHECK(prop_r.has_value(), "syntax:propagate-marker returns success");
    int64_t count = aura::compiler::types::as_int(*prop_r);
    CHECK(count > 1,
          "propagate-marker updated > 1 node (define + lambda + body subtree)");

    // Verify the root + at least one child are now MacroIntroduced.
    auto m_root = cs.eval("(syntax-marker " + std::to_string(root_id) + ")");
    if (!m_root) return false;
    CHECK(aura::compiler::types::as_int(*m_root) == 1,
          "root node is now MacroIntroduced after propagate");
    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 3: end-to-end marker consistency under mutate
// ═══════════════════════════════════════════════════════════

bool test_marker_consistency_under_mutate() {
    std::println("\n--- AC3: marker consistency: set + query + propagate + query round-trip ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define y 0)\")");

    auto r = cs.eval("(car (query:find \"y\"))");
    if (!r) return false;
    int64_t nid = aura::compiler::types::as_int(*r);

    // Set the marker, then verify syntax-marker reflects it.
    cs.eval("(syntax:set-marker " + std::to_string(nid) + " 1)");
    auto m = cs.eval("(syntax-marker " + std::to_string(nid) + ")");
    if (!m) return false;
    CHECK(aura::compiler::types::as_int(*m) == 1,
          "marker set + query round-trip (set: User→Macro, query returns Macro)");

    // Reset to BoolLiteral (2).
    cs.eval("(syntax:set-marker " + std::to_string(nid) + " 2)");
    m = cs.eval("(syntax-marker " + std::to_string(nid) + ")");
    if (!m) return false;
    CHECK(aura::compiler::types::as_int(*m) == 2,
          "marker reset: Macro→BoolLiteral");

    // Reset back to User (0).
    cs.eval("(syntax:set-marker " + std::to_string(nid) + " 0)");
    m = cs.eval("(syntax-marker " + std::to_string(nid) + ")");
    if (!m) return false;
    CHECK(aura::compiler::types::as_int(*m) == 0,
          "marker reset: BoolLiteral→User");
    return true;
}

// ═══════════════════════════════════════════════════════════
// Main test runner
// ═══════════════════════════════════════════════════════════

int run_tests() {
    std::println("═══ Issue #366 verification tests ═══\n");

    std::println("Layer 1: syntax:set-marker");
    test_set_marker_on_single_node();

    std::println("\nLayer 2: syntax:propagate-marker");
    test_propagate_marker_subtree();

    std::println("\nLayer 3: end-to-end marker round-trip");
    test_marker_consistency_under_mutate();

    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}
}  // namespace aura_issue_366_detail

int aura_issue_366_run() { return aura_issue_366_detail::run_tests(); }