// @category: integration
// @reason: uses CompilerService to eval Aura source
// test_issue_211.cpp — Issue #211 dedicated tree pattern
// matcher: (tag, arity) index for query:pattern.
//
// Verifies that the (tag, arity) index is built on demand
// and used to skip non-matching nodes in query:pattern:
//
//   1. The index is empty before any query:pattern call
//      (lazy build).
//   2. After force_build_tag_arity_index(), the index is
//      populated (verifies the build logic itself).
//   3. The index workspace ptr matches workspace_flat_
//      after build.
//   4. After a second call, the index is reused (no
//      rebuild) — the same ptr is set.
//   5. After invalidate_tag_arity_index, the index is
//      empty and workspace ptr is null.
//   6. After invalidate + set_workspace_flat on a new
//      workspace, the next force_build rebuilds for
//      the new workspace.
//
// Note: the actual query:pattern integration (using
// the index in the walk loop) is tested by the
// run-tests.sh script (which exercises query:pattern
// via the Aura CLI). The unit tests here focus on
// the index lifecycle and API.
//
// The variadic `...`, predicates, and pattern memoization
// are out of scope for this PR (documented as follow-ups
// in the issue body).

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core;
import aura.compiler.value;



#define PRINTLN(msg) std::fprintf(stdout, "%s\n", (msg))

// ── Test 1: index is empty before any build ──
// The index is lazy-built. Without a workspace, no
// build happens (and the index stays empty).
bool test_index_empty_without_workspace() {
    PRINTLN("\n--- Test 1: index is empty without workspace ---");
    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();
    // No set-code called, no workspace loaded.
    CHECK(ev.workspace_flat() == nullptr,
          "no workspace loaded");
    // Force-build the index (no-op since no workspace).
    ev.force_build_tag_arity_index();
    CHECK(ev.tag_arity_index_size() == 0,
          "index is empty (no workspace to index)");
    CHECK(ev.tag_arity_index_workspace() == nullptr,
          "index workspace ptr is null");
    return true;
}

// ── Test 2: index is populated when workspace is loaded ──
// After loading a workspace via set_workspace_flat and
// force-building the index, the index should be populated.
bool test_index_populated_after_build() {
    PRINTLN("\n--- Test 2: index is populated after force_build ---");
    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();
    // Allocate a workspace FlatAST + pool
    auto alloc = ev.test_arena().allocator();
    auto* pool = ev.test_arena().create<aura::ast::StringPool>(alloc);
    auto* flat = ev.test_arena().create<aura::ast::FlatAST>(alloc);
    // Populate with a few nodes using add_node helpers
    auto x_sym = pool->intern("x");
    flat->add_variable(x_sym);
    flat->add_literal(42);
    // Set the workspace
    ev.set_workspace_flat_for_test(flat);
    // Build the index
    ev.force_build_tag_arity_index();
    // The index should have entries (one per unique
    // (tag, arity) pair).
    CHECK(ev.tag_arity_index_size() >= 1,
          "index is populated after force_build");
    CHECK(ev.tag_arity_index_workspace() == flat,
          "index workspace ptr matches the loaded workspace");
    return true;
}

// ── Test 3: index is reused across calls (no rebuild) ──
// After the first build, subsequent calls to
// force_build should be no-ops (the index is cached).
bool test_index_reused_across_calls() {
    PRINTLN("\n--- Test 3: index is reused across calls ---");
    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();
    // Setup workspace (reuse test 2's setup)
    auto alloc = ev.test_arena().allocator();
    auto* pool = ev.test_arena().create<aura::ast::StringPool>(alloc);
    auto* flat = ev.test_arena().create<aura::ast::FlatAST>(alloc);
    auto x_sym = pool->intern("x");
    flat->add_variable(x_sym);
    ev.set_workspace_flat_for_test(flat);
    // First build
    ev.force_build_tag_arity_index();
    const std::size_t size_after_first = ev.tag_arity_index_size();
    const auto* ptr_after_first = ev.tag_arity_index_workspace();
    // Second build (no-op since the workspace ptr matches)
    ev.force_build_tag_arity_index();
    CHECK(ev.tag_arity_index_size() == size_after_first,
          "index size unchanged after second force_build (no rebuild)");
    CHECK(ev.tag_arity_index_workspace() == ptr_after_first,
          "index workspace ptr unchanged after second force_build");
    return true;
}

// ── Test 4: invalidate clears the index ──
// After invalidate, the index is empty and the workspace
// ptr is null. The next force_build will rebuild.
bool test_invalidate_clears_index() {
    PRINTLN("\n--- Test 4: invalidate clears the index ---");
    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();
    // Setup and build
    auto alloc = ev.test_arena().allocator();
    auto* pool = ev.test_arena().create<aura::ast::StringPool>(alloc);
    auto* flat = ev.test_arena().create<aura::ast::FlatAST>(alloc);
    auto x_sym = pool->intern("x");
    flat->add_variable(x_sym);
    ev.set_workspace_flat_for_test(flat);
    ev.force_build_tag_arity_index();
    CHECK(ev.tag_arity_index_size() >= 1, "index built");
    // Invalidate
    ev.invalidate_tag_arity_index_for_test();
    CHECK(ev.tag_arity_index_size() == 0,
          "index empty after invalidate");
    CHECK(ev.tag_arity_index_workspace() == nullptr,
          "workspace ptr null after invalidate");
    return true;
}

// ── Test 5: rebuild after invalidate uses the new workspace ──
// After invalidate, the next force_build rebuilds
// from the current workspace. This verifies the
// rebuild logic uses the CURRENT workspace (not a
// stale one).
bool test_rebuild_after_invalidate_uses_current_workspace() {
    PRINTLN("\n--- Test 5: rebuild after invalidate uses current workspace ---");
    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();
    // Setup first workspace
    auto alloc = ev.test_arena().allocator();
    auto* pool1 = ev.test_arena().create<aura::ast::StringPool>(alloc);
    auto* flat1 = ev.test_arena().create<aura::ast::FlatAST>(alloc);
    auto x1 = pool1->intern("x");
    flat1->add_variable(x1);
    ev.set_workspace_flat_for_test(flat1);
    ev.force_build_tag_arity_index();
    CHECK(ev.tag_arity_index_workspace() == flat1,
          "first build: workspace ptr is flat1");
    // Invalidate and switch to a new workspace
    ev.invalidate_tag_arity_index_for_test();
    auto* pool2 = ev.test_arena().create<aura::ast::StringPool>(alloc);
    auto* flat2 = ev.test_arena().create<aura::ast::FlatAST>(alloc);
    auto y2 = pool2->intern("y");
    flat2->add_variable(y2);
    ev.set_workspace_flat_for_test(flat2);
    // Rebuild — should use flat2
    ev.force_build_tag_arity_index();
    CHECK(ev.tag_arity_index_workspace() == flat2,
          "rebuild: workspace ptr is flat2 (new workspace)");
    return true;
}

int main() {
    std::fprintf(stdout, "═══ Issue #211 — (tag, arity) index for query:pattern ═══\n");
    std::fprintf(stdout, "  Verifies the index is built, cached, and invalidated.\n");
    std::fprintf(stdout, "  Variadic ..., predicates, and memoization are follow-ups.\n\n");

    test_index_empty_without_workspace();
    test_index_populated_after_build();
    test_index_reused_across_calls();
    test_invalidate_clears_index();
    test_rebuild_after_invalidate_uses_current_workspace();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
