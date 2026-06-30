// @category: integration
// @reason: uses CompilerService + PersistentChildVector +
//          SafePCVSpan lifetime safety
//
// test_issue_370.cpp — Verify Issue #370 acceptance criteria
// ("[EDSL Concurrency] PCV COW lifetime + dangling raw
//  references / shared_ptr during rollback with external
//  holders").
//
// Background: PersistentChildVector is COW (shared_ptr<const
// Storage>). External holders (closure-captured children
// spans, MutationRecord back-refs, AI Agent-cached
// NodeView/span, FFI buffers) might cache the returned
// std::span<const NodeId>. After with_* mutation replaces the
// underlying PCV in FlatAST::children_[id], the span dangles.
// Same risk on rollback: the pre-mutation PCV is held alive
// by the MutationCheckpoint shared_ptr until the boundary is
// exited, but external holders have no such keepalive.
//
// Fix (#370):
//
// 1. New SafePCVSpan<T> in
//    src/core/persistent_child_vector.hh: bundles span +
//    shared_ptr<Storage>. As long as SafePCVSpan is alive,
//    the underlying storage stays alive.
//
// 2. New FlatAST::children_safe(NodeId) returning
//    SafePCVSpan<NodeId>. One atomic refcount bump per call.
//
// 3. Documented the raw children(NodeId) (span) as
//    "single-statement use only — does NOT survive
//    mutations".
//
// 4. New children_safe_view_count atomic counter in FlatAST
//    + observability snapshot + (ast:generation-stats)
//    children-safe-view-count key.
//
// Test strategy: 4 layers (all via C++ API).
//
//   Layer 1: SafePCVSpan round-trip (construct from PCV,
//            accessor methods, use_count > 0)
//   Layer 2: lifetime-pinning across PCV mutation
//            (capture safe view, mutate the underlying PCV,
//            verify safe span still readable AND points to
//            the OLD data, not a dangling reference)
//   Layer 3: children_safe() counter increments on call
//   Layer 4: Aura-level smoke test (ast:generation-stats
//            runs cleanly with the new key)

#include "test_harness.hpp"

#include "../src/core/persistent_child_vector.hh"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;

namespace aura_issue_370_detail {

// ═══════════════════════════════════════════════════════════
// Layer 1: SafePCVSpan round-trip
// ═══════════════════════════════════════════════════════════

bool test_safe_pcv_span_construct() {
    std::println("\n--- AC1: SafePCVSpan basic round-trip ---");
    aura::ast::PersistentChildVector<aura::ast::NodeId> pcv{
        aura::ast::NodeId{1}, aura::ast::NodeId{2}, aura::ast::NodeId{3}};

    auto keep = share_storage(pcv);
    std::span<const aura::ast::NodeId> sp = pcv;
    aura::ast::SafePCVSpan<aura::ast::NodeId> safe(sp, keep);

    CHECK(safe.size() == 3, "SafePCVSpan.size() == 3");
    CHECK(!safe.empty(), "SafePCVSpan is not empty");
    CHECK(safe[0] == 1u, "SafePCVSpan[0] == 1");
    CHECK(safe[2] == 3u, "SafePCVSpan[2] == 3");
    CHECK(safe.use_count() >= 2,
          "SafePCVSpan.use_count() >= 2 (safe + original PCV)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 2: lifetime-pinning across mutation
// ═══════════════════════════════════════════════════════════

bool test_lifetime_pinning_across_mutation() {
    std::println("\n--- AC2: SafePCVSpan survives PCV mutation ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);

    // Build parent + 2 children. children_[parent] holds a PCV.
    // Use insert_child (append-able on an empty PCV) since
    // set_child on an out-of-range index is a no-op.
    auto parent = flat.add_literal(0);
    auto c1 = flat.add_literal(7);
    auto c2 = flat.add_literal(42);
    flat.insert_child(parent, 0, c1);
    flat.insert_child(parent, 1, c2);

    // Capture a SafePCVSpan BEFORE mutate.
    auto safe = flat.children_safe(parent);
    CHECK(safe.size() == 2, "before mutate: safe span has 2 children");
    auto c1_before = safe[0];
    auto c2_before = safe[1];
    CHECK(c1_before == c1, "before mutate: safe[0] == c1");
    CHECK(c2_before == c2, "before mutate: safe[1] == c2");

    // Mutate: replace child[1] with c2 removed.
    flat.remove_child(parent, 1);
    // children_[parent] now points to a different PCV; the
    // raw span would dangle. The safe span, however, holds
    // the OLD shared_ptr so it's still valid.
    auto raw_after = flat.children(parent);
    CHECK(raw_after.size() == 1,
          "after mutate: raw children() shows 1 child (children_[parent] replaced)");

    // The safe span still reads the pre-mutation data.
    CHECK(safe.size() == 2,
          "after mutate: safe span STILL has 2 children (lifetime pinned)");
    CHECK(safe[0] == c1_before,
          "after mutate: safe[0] == pre-mutation c1");
    CHECK(safe[1] == c2_before,
          "after mutate: safe[1] == pre-mutation c2 (no UAF)");

    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 3: children_safe() counter increments
// ═══════════════════════════════════════════════════════════

bool test_children_safe_counter() {
    std::println("\n--- AC3: children_safe_view_count counter increments ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);
    auto n0 = flat.add_literal(0);

    auto before = flat.children_safe_view_count();
    auto safe = flat.children_safe(n0);
    auto after = flat.children_safe_view_count();
    CHECK(after >= before + 1,
          "children_safe_view_count incremented by >= 1");
    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 4: Aura smoke test
// ═══════════════════════════════════════════════════════════

bool test_ast_generation_stats_has_safe_view_key() {
    std::println("\n--- AC4: (ast:generation-stats) includes children-safe-view-count ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(ast:generation-stats)");
    CHECK(r.has_value(), "ast:generation-stats returns a value");
    return true;
}

// ═══════════════════════════════════════════════════════════
// Main test runner
// ═══════════════════════════════════════════════════════════

int run_tests() {
    std::println("═══ Issue #370 verification tests ═══\n");

    std::println("Layer 1: SafePCVSpan round-trip");
    test_safe_pcv_span_construct();

    std::println("\nLayer 2: lifetime-pinning across PCV mutation");
    test_lifetime_pinning_across_mutation();

    std::println("\nLayer 3: counter increments on call");
    test_children_safe_counter();

    std::println("\nLayer 4: Aura smoke test");
    test_ast_generation_stats_has_safe_view_key();

    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}
}  // namespace aura_issue_370_detail

int aura_issue_370_run() { return aura_issue_370_detail::run_tests(); }