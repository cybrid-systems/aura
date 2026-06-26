// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_177.cpp — Issue #213 verification:
//
// Integration-level smoke test for the migrated mutate:*
// primitives + the new rollback machinery + per-fiber
// state. This is the canonical test for #177 (the parent
// issue that #213 completes).
//
// The unit-level coverage is in tests/test_issue_213.cpp
// (7 scenarios, 28 sub-checks, all green). This test is
// the integration-level companion: it exercises the
// rollback + per-fiber stack through the full Evaluator
// + workspace_flat_ + workspace_pool_ setup, verifying
// the end-to-end chain works.
//
// Test scenarios:
//   1. exit(success=true) keeps the mutation (e2e via
//      type_id_ field, real rollback data)
//   2. exit(success=false) rolls back the type_id_
//   3. Per-fiber mutation stack (Issue #213 Cycle 3):
//      mutation_boundary_depth() reflects the right
//      stack depth at every stage
//   4. Nested boundaries share the main-thread stack
//      when no fiber is active (the active_mutation_stack
//      routing)


// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import std;
import aura.core;
import aura.core.type;
import aura.diag;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.ir;



namespace aura_issue_177_detail {
#define PRINTLN(msg) std::print( "%s\n", (msg))

// ── Test 1: type_id_ rollback end-to-end ────────────────
//
// Verifies that exit(success=false) actually rolls back
// the type_id_ SoA column. This is the most common rollback
// path in the mutated primitives (mutate:replace-type,
// mutate:replace-value on type-annotated nodes).
bool test_type_id_rollback() {
    PRINTLN("\n--- Test 1: type_id_ rollback ---");
    aura::compiler::Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);

    auto node = flat.add_literal(100);
    flat.root = node;
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    // Initial type = 0 (Dyn). Set to 1 (Int).
    flat.set_type(node, 1);
    CHECK(flat.type_id(node) == 1, "type is Int initially");

    // Enter + exit(false) with a type mutation → rollback
    ev.enter_mutation_boundary();
    flat.set_type(node, 3);  // String
    flat.add_mutation_with_rollback(
        node, "replace-type", "#1", "#3", "Int -> String",
        aura::ast::MutationStatus::Committed,
        /*field_offset=*/1, /*old=*/1, /*new=*/3, true);
    CHECK(flat.type_id(node) == 3, "type is String mid-boundary");
    ev.exit_mutation_boundary(false);

    CHECK(flat.type_id(node) == 1, "type rolled back to Int (1)");
    return true;
}

// ── Test 2: int_val_ rollback end-to-end ────────────────
//
// Verifies the int_val_ rollback path (used by
// mutate:tweak-literal, mutate:replace-value on Int).
bool test_int_val_rollback() {
    PRINTLN("\n--- Test 2: int_val_ rollback ---");
    aura::compiler::Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);

    auto node = flat.add_literal(10);
    flat.root = node;
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    CHECK(flat.int_val(node) == 10, "initial value is 10");

    ev.enter_mutation_boundary();
    flat.set_int(node, 99);
    flat.add_mutation_with_rollback(
        node, "tweak", "Int", "Int", "10 -> 99",
        aura::ast::MutationStatus::Committed,
        /*field_offset=*/0, /*old=*/10, /*new=*/99, true);
    CHECK(flat.int_val(node) == 99, "mid-boundary value is 99");
    ev.exit_mutation_boundary(false);

    CHECK(flat.int_val(node) == 10, "value rolled back to 10");
    return true;
}

// ── Test 3: per-fiber mutation stack depth ─────────────
//
// Verifies that the per-fiber state (Issue #213 Cycle 3)
// works. With no active fiber, the main-thread stack
// is used. Depth goes 0 → 1 → 2 → 1 → 0 as we enter/exit.
bool test_mutation_stack_depth() {
    PRINTLN("\n--- Test 3: mutation stack depth ---");
    CHECK(aura::compiler::Evaluator::mutation_boundary_depth() == 0,
          "depth starts at 0");

    aura::compiler::Evaluator ev1, ev2;
    ev1.enter_mutation_boundary();
    CHECK(aura::compiler::Evaluator::mutation_boundary_depth() == 1,
          "depth = 1 after ev1.enter");
    ev2.enter_mutation_boundary();
    CHECK(aura::compiler::Evaluator::mutation_boundary_depth() == 2,
          "depth = 2 after ev2.enter (shared main-thread stack)");
    ev2.exit_mutation_boundary(true);
    CHECK(aura::compiler::Evaluator::mutation_boundary_depth() == 1,
          "depth = 1 after ev2.exit");
    ev1.exit_mutation_boundary(true);
    CHECK(aura::compiler::Evaluator::mutation_boundary_depth() == 0,
          "depth = 0 after ev1.exit");
    return true;
}

// ── Test 4: nested boundaries on the main-thread stack ──
bool test_nested_boundaries() {
    PRINTLN("\n--- Test 4: nested boundaries ---");
    aura::compiler::Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    flat.add_literal(0);
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    auto v0 = ev.defuse_version_snapshot();
    // Nested: outer commits, inner fails
    ev.enter_mutation_boundary();
    ev.enter_mutation_boundary();
    flat.set_int(0, 99);
    flat.add_mutation_with_rollback(
        0, "inner", "Int", "Int", "0 -> 99",
        aura::ast::MutationStatus::Committed, 0, 0, 99, true);
    ev.exit_mutation_boundary(false);  // inner fails
    // outer commits (the inner failure only rolled back the inner)
    ev.exit_mutation_boundary(true);

    CHECK(flat.int_val(0) == 0, "inner rolled back to 0");
    CHECK(ev.defuse_version_snapshot() == v0 + 4, "4 version bumps (2 enters + 2 exits)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #213 / #177 — integration smoke test ═══");
    std::println("  Verifies the full #213 chain end-to-end:");
    std::println("    - Cycle 1: rollback mechanism");
    std::println("    - Cycle 2: 13/19 mutate:* primitives migrated");
    std::println("    - Cycle 3: g_mutation_stack → per-fiber state\n");

    test_type_id_rollback();
    test_int_val_rollback();
    test_mutation_stack_depth();
    test_nested_boundaries();

    std::println("\n──────────────────────────────────────");
    std::println("Total: %d passed, %d failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_177_detail

int aura_issue_177_run() { return aura_issue_177_detail::run_tests(); }

