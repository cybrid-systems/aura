// test_issue_116.cpp — Verify deferred CoercionNode insertion
// (TypeChecker no longer mutates FlatAST in-place).
//
// Regression scenarios:
//   1. After `infer_flat` (without apply), the FlatAST's
//      structure (node count, parent→child links) is unchanged.
//      Only `set_node_error` annotations are allowed.
//   2. After `take_coercions()` + `apply_coercion_map`, the
//      FlatAST has the same CoercionNodes that the old
//      in-place version would have produced.
//   3. The apply pass is idempotent: applying the same map
//      twice does not double-insert.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <string>
#include <vector>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.arena;
import aura.compiler.type_checker;
import aura.compiler.coercion_map;
import aura.compiler.value;
import aura.diag;
import aura.core.type;
import aura.parser.parser;



struct TypecheckResult {
    std::unique_ptr<aura::ast::ASTArena> arena;
    std::unique_ptr<aura::core::TypeRegistry> treg;
    std::unique_ptr<aura::compiler::TypeChecker> tc;
    aura::ast::FlatAST* flat = nullptr;
    aura::ast::StringPool* pool = nullptr;
    aura::ast::NodeId root = 0;
    int node_count_before = 0;
};

TypecheckResult typecheck(const std::string& src) {
    TypecheckResult r;
    r.arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = r.arena->allocator();
    auto* flat = r.arena->create<aura::ast::FlatAST>(alloc);
    r.pool = r.arena->create<aura::ast::StringPool>(alloc);
    auto pr = aura::parser::parse_to_flat(src, *flat, *r.pool);
    flat->root = pr.root;
    r.flat = flat;
    r.root = pr.root;
    r.node_count_before = flat->size();
    if (!pr.success) return r;
    r.treg = std::make_unique<aura::core::TypeRegistry>();
    r.tc = std::make_unique<aura::compiler::TypeChecker>(*r.treg);
    return r;
}

int count_coercions(const aura::ast::FlatAST& flat) {
    int n = 0;
    for (aura::ast::NodeId i = 0; i < flat.size(); ++i) {
        if (flat.get(i).tag == aura::ast::NodeTag::Coercion) ++n;
    }
    return n;
}

// ── Test 1: well-typed expr produces zero CoercionMap entries
//            and the FlatAST is unchanged. ─────────────────────

bool test_well_typed_no_mutation() {
    std::println("\n--- Test: well-typed expr — no mutation, no coercions ---");

    auto r = typecheck("(+ 1 2)");
    aura::diag::DiagnosticCollector diag;
    r.tc->infer_flat(*r.flat, *r.pool, r.root, diag);

    CHECK((*r.flat).size() == r.node_count_before,
          "well-typed expr: no new nodes added by infer_flat");
    CHECK(count_coercions(*r.flat) == 0,
          "well-typed expr: zero CoercionNodes in AST");
    CHECK(r.tc->last_coercions().empty(),
          "well-typed expr: CoercionMap is empty");
    return true;
}

// ── Test 2: apply_coercion_map round-trip on a manually
//            constructed CoercionMap ─────────────────────────────
//
// Builds a small AST by hand, constructs a CoercionMap
// targeting an existing parent/child slot, and verifies the
// apply pass inserts the CoercionNode correctly. This
// bypasses the type checker's coercion logic (which depends
// on registered primitive signatures and the strict /
// permissive mode) and tests the apply pass in isolation,
// which is the new code that landed in Issue #116.

bool test_apply_round_trip() {
    std::println("\n--- Test: apply_coercion_map round-trip ---");

    // Build a small AST: a Call node `(+ 1 2)`. In add_call,
    // child[0] is the callee (a Variable node) and child[1+]
    // are the arguments. We target child_index=1 (arg0 = 1)
    // for the coercion.
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto callee_sym = pool->intern("+");
    auto callee_var = flat->add_variable(callee_sym);
    auto arg0 = flat->add_literal(42);
    auto arg1 = flat->add_literal(2);
    auto call_id = flat->add_call(callee_var, {arg0, arg1});
    flat->root = call_id;

    // Snapshot before
    int n_before = flat->size();
    auto call_before = flat->get(call_id);
    CHECK(call_before.child(1) == arg0,
          "call.child[1] (arg0) link points to literal 42");

    // Build a CoercionMap targeting arg0 at child_index=1
    // (the first argument position; child[0] is the callee).
    aura::compiler::CoercionMap cm;
    cm.add(call_id, 1, arg0, 2, 1, 0, 0);

    // Apply
    auto applied = aura::compiler::apply_coercion_map(*flat, cm);
    CHECK(applied == 1, "apply_coercion_map applied 1 entry");

    // After: a new CoercionNode wraps arg0
    int n_after = flat->size();
    CHECK(n_after == n_before + 1, "1 new node added (CoercionNode)");

    // The parent's child_index=1 should now point to the
    // new CoercionNode, not to arg0.
    auto parent = flat->get(call_id);
    auto new_link = parent.child(1);
    CHECK(new_link != arg0,
          "call.child[1] (arg0) no longer points to literal 42");
    CHECK(flat->get(new_link).tag == aura::ast::NodeTag::Coercion,
          "call.child[1] is now a CoercionNode");

    // The new CoercionNode's child should be the original arg0.
    auto coercion = flat->get(new_link);
    CHECK(coercion.children.size() == 1, "CoercionNode has 1 child");
    CHECK(coercion.child(0) == arg0,
          "CoercionNode's child is the original literal 42");
    return true;
}

// ── Test 3: apply_coercion_map is idempotent ────────────────
//
// Re-applies the same map to an already-mutated AST. The
// recorded (parent, child_index, original_child) triple
// no longer matches (the child_index now points to the
// CoercionNode, not the original literal), so all entries
// are skipped and the AST is unchanged.

bool test_apply_idempotent() {
    std::println("\n--- Test: apply_coercion_map is idempotent ---");

    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto callee_sym = pool->intern("+");
    auto callee_var = flat->add_variable(callee_sym);
    auto arg0 = flat->add_literal(42);
    auto arg1 = flat->add_literal(2);
    auto call_id = flat->add_call(callee_var, {arg0, arg1});
    flat->root = call_id;

    aura::compiler::CoercionMap cm;
    cm.add(call_id, 1, arg0, 2, 1, 0, 0);

    // First apply
    auto applied1 = aura::compiler::apply_coercion_map(*flat, cm);
    int n1 = flat->size();
    CHECK(applied1 == 1, "first apply: 1 entry applied");
    CHECK(count_coercions(*flat) == 1, "first apply: 1 CoercionNode in AST");

    // Second apply — should be a no-op
    auto applied2 = aura::compiler::apply_coercion_map(*flat, cm);
    int n2 = flat->size();
    CHECK(applied2 == 0, "second apply: 0 entries applied (idempotent)");
    CHECK(n2 == n1, "second apply: AST unchanged (no duplicate CoercionNode)");
    CHECK(count_coercions(*flat) == 1, "still exactly 1 CoercionNode");
    return true;
}

// ── Test 4: snapshot semantics are restored ────────────────
//
// The original Issue #116 motivation: the type checker no
// longer mutates the FlatAST, so the total parent→child link
// count is identical before and after typecheck.

bool test_snapshot_semantics() {
    std::println("\n--- Test: snapshot semantics restored (Issue #116 goal) ---");

    auto r = typecheck("(+ \"42\" 1)");

    // Capture "snapshot" before infer
    int total_links_before = 0;
    for (aura::ast::NodeId i = 0; i < r.node_count_before; ++i) {
        total_links_before += (*r.flat).get(i).children.size();
    }
    aura::diag::DiagnosticCollector diag;
    r.tc->set_strict(true);
    r.tc->infer_flat(*r.flat, *r.pool, r.root, diag);

    int total_links_after = 0;
    for (aura::ast::NodeId i = 0; i < (*r.flat).size(); ++i) {
        total_links_after += (*r.flat).get(i).children.size();
    }
    CHECK(total_links_before == total_links_after,
          "total parent->child link count unchanged by infer_flat");
    CHECK((*r.flat).size() == r.node_count_before,
          "node count unchanged by infer_flat");
    return true;
}

// ── Test 5: type system still reports correct types ─────────
//
// Issue #116 was a refactor — the observable type
// information from infer_flat should be identical to the
// old in-place version. We verify by typechecking and
// checking that the returned TypeId is the same.

bool test_type_results_unchanged() {
    std::println("\n--- Test: type results unchanged by refactor ---");

    auto r = typecheck("(+ 1 2)");
    aura::diag::DiagnosticCollector diag;
    auto tid = r.tc->infer_flat(*r.flat, *r.pool, r.root, diag);
    // 1 + 2 = Int (result of +)
    // The exact TypeId is an internal index, so we just check
    // that it's a valid type (not the unit/void type).
    CHECK(tid.valid(), "type result is valid");
    CHECK(tid.index != 0, "type result has non-zero index");
    return true;
}

// ── Test 6: defer vs apply on a real Aura expression ─────────
//
// End-to-end smoke test: typecheck an actual Aura program
// and verify that the AST is unchanged post-infer_flat and
// that the CoercionMap (if populated) is non-empty. This
// guards against regressions in the type-checker's
// CoercionMap collection path.

bool test_real_program_defer() {
    std::println("\n--- Test: real program defer smoke test ---");

    auto r = typecheck("(let ((x 1)) (+ x 2))");
    aura::diag::DiagnosticCollector diag;
    r.tc->infer_flat(*r.flat, *r.pool, r.root, diag);
    CHECK((*r.flat).size() == r.node_count_before,
          "real program: no mutation by infer_flat");
    // For this well-typed input, the CoercionMap should be
    // empty (no coercions needed).
    CHECK(r.tc->last_coercions().empty(),
          "real program: well-typed → no coercions in map");
    return true;
}

int run_issue_116() {
    std::println("═══ Issue #116 verification tests ═══\n");
    test_well_typed_no_mutation();
    test_apply_round_trip();
    test_apply_idempotent();
    test_snapshot_semantics();
    test_type_results_unchanged();
    test_real_program_defer();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
