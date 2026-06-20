// @category: regression
// @reason: mentions 'regression' in top comment
// test_issue_126.cpp — Verify the pure functions extracted
// from CompilerService and Evaluator (Issue #126).
//
// Regression scenarios:
//   1. should_relower — covers all 5 input combinations
//      (clean, dirty, hash-mismatch, mutation-drift, mixed)
//   2. compute_dependencies — walks FlatAST, returns
//      deduplicated, first-encounter-order list
//   3. try_extract_define — pattern matches Define nodes,
//      returns nullopt for non-Define roots
//   4. fnv1a_64 — known answer test for FNV-1a hash

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <string>
#include <vector>
#include <unordered_set>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.arena;
import aura.compiler.ir_cache_pure;



// ── Test 1: should_relower covers all combinations ─────

namespace aura_issue_126_detail {
bool test_should_relower() {
    std::println("\n--- Test: should_relower — pure decision function ---");

    // Clean entry, hash matches, no mutation drift → no re-lower
    CHECK(!aura::compiler::should_relower(/*src_hash=*/100, /*cached=*/100,
                                          /*dirty=*/false,
                                          /*cached_mc=*/5, /*curr_mc=*/5),
          "clean, hash match, no mutation drift → no re-lower");

    // Dirty entry → re-lower
    CHECK(aura::compiler::should_relower(/*src_hash=*/100, /*cached=*/100,
                                          /*dirty=*/true,
                                          /*cached_mc=*/5, /*curr_mc=*/5),
          "dirty → re-lower");

    // Hash mismatch → re-lower
    CHECK(aura::compiler::should_relower(/*src_hash=*/100, /*cached=*/200,
                                          /*dirty=*/false,
                                          /*cached_mc=*/5, /*curr_mc=*/5),
          "hash mismatch → re-lower");

    // Mutation drift → re-lower
    CHECK(aura::compiler::should_relower(/*src_hash=*/100, /*cached=*/100,
                                          /*dirty=*/false,
                                          /*cached_mc=*/3, /*curr_mc=*/5),
          "mutation drift (cached<current) → re-lower");

    // Mixed: dirty + hash mismatch + drift → re-lower
    CHECK(aura::compiler::should_relower(/*src_hash=*/100, /*cached=*/200,
                                          /*dirty=*/true,
                                          /*cached_mc=*/1, /*curr_mc=*/5),
          "dirty + hash mismatch + drift → re-lower");

    // Edge case: equal hashes but mutation_count back to 0
    CHECK(!aura::compiler::should_relower(/*src_hash=*/0, /*cached=*/0,
                                          /*dirty=*/false,
                                          /*cached_mc=*/0, /*curr_mc=*/0),
          "all-zero inputs → no re-lower");
    return true;
}

// ── Test 2: fnv1a_64 known answer test ─────────────────

bool test_fnv1a_64() {
    std::println("\n--- Test: fnv1a_64 — pure hash function ---");

    // Known FNV-1a 64-bit values (from the standard test vectors):
    //   "" → 0xcbf29ce484222325
    //   "a" → 0xaf63dc4c8601ec8c
    //   "foobar" → 0x85944171f73967e8
    auto h0 = aura::compiler::fnv1a_64("");
    auto h1 = aura::compiler::fnv1a_64("a");
    auto h_foo = aura::compiler::fnv1a_64("foobar");

    CHECK(h0 == 0xcbf29ce484222325ULL,
          "fnv1a_64(\"\") = 0xcbf29ce484222325");
    CHECK(h1 == 0xaf63dc4c8601ec8cULL,
          "fnv1a_64(\"a\") = 0xaf63dc4c8601ec8c");
    CHECK(h_foo == 0x85944171f73967e8ULL,
          "fnv1a_64(\"foobar\") = 0x85944171f73967e8");

    // Same input → same output (purity)
    CHECK(aura::compiler::fnv1a_64("hello") == aura::compiler::fnv1a_64("hello"),
          "fnv1a_64 is deterministic");
    CHECK(aura::compiler::fnv1a_64("hello") != aura::compiler::fnv1a_64("Hello"),
          "fnv1a_64 is case-sensitive");
    return true;
}

// ── Test 3: compute_dependencies walks the AST ──────────

bool test_compute_dependencies() {
    std::println("\n--- Test: compute_dependencies — pure AST walker ---");

    // Build a tiny AST: (+ a b) where a and b are variables.
    // Available defines: { "a", "b", "c" }.
    // Expected: dependencies are { "a", "b" } in that order.
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);
    aura::ast::StringPool pool(alloc);
    auto a_id = pool.intern("a");
    auto b_id = pool.intern("b");
    auto a_var = flat.add_variable(a_id);
    auto b_var = flat.add_variable(b_id);
    // add_call(func, args) — func is a NodeId, not SymId
    auto plus_id = pool.intern("+");
    auto plus_var = flat.add_variable(plus_id);
    auto call = flat.add_call(plus_var, {a_var, b_var});

    std::unordered_set<std::string> available;
    available.insert("a");
    available.insert("b");
    available.insert("c");

    auto deps = aura::compiler::compute_dependencies(flat, pool, call, available);
    CHECK(deps.size() == 2, "compute_dependencies returns 2 deps for (a b)");
    if (deps.size() == 2) {
        CHECK(deps[0] == "a", "first dep is 'a'");
        CHECK(deps[1] == "b", "second dep is 'b'");
    }

    // Deduplication: call appears twice with same var.
    auto a2_var = flat.add_variable(a_id);
    auto call_dup = flat.add_call(plus_var, {a_var, a2_var});
    auto deps2 = aura::compiler::compute_dependencies(flat, pool, call_dup, available);
    CHECK(deps2.size() == 1, "compute_dependencies deduplicates 'a'");
    if (deps2.size() == 1) {
        CHECK(deps2[0] == "a", "deduplicated dep is 'a'");
    }

    // d is not in available_defines → not in deps
    auto d_id = pool.intern("d");
    auto d_var = flat.add_variable(d_id);
    auto call3 = flat.add_call(plus_var, {a_var, d_var});
    auto deps3 = aura::compiler::compute_dependencies(flat, pool, call3, available);
    CHECK(deps3.size() == 1 && deps3[0] == "a",
          "compute_dependencies excludes 'd' (not in available_defines)");

    // Empty available set → empty deps
    std::unordered_set<std::string> empty;
    auto deps4 = aura::compiler::compute_dependencies(flat, pool, call, empty);
    CHECK(deps4.empty(), "compute_dependencies with empty available returns empty");
    return true;
}

// ── Test 4: try_extract_define pattern matches ─────────

bool test_try_extract_define() {
    std::println("\n--- Test: try_extract_define — pure AST pattern match ---");

    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);
    aura::ast::StringPool pool(alloc);
    auto name_id = pool.intern("my-def");
    auto body_id = flat.add_literal(42);
    auto define_id = flat.add_define(name_id, body_id);

    // The root IS a Define node
    auto def = aura::compiler::try_extract_define(flat, pool, define_id);
    CHECK(def.has_value(), "try_extract_define returns Some for Define root");
    if (def) {
        CHECK(def->first == "my-def", "extracted name is 'my-def'");
        CHECK(def->second == body_id, "extracted body is the literal 42");
    }

    // The root is NOT a Define node (just a literal)
    auto lit = flat.add_literal(99);
    auto def2 = aura::compiler::try_extract_define(flat, pool, lit);
    CHECK(!def2.has_value(),
          "try_extract_define returns None for non-Define root");

    // NULL_NODE → nullopt
    auto def3 = aura::compiler::try_extract_define(flat, pool, aura::ast::NULL_NODE);
    CHECK(!def3.has_value(), "try_extract_define returns None for NULL_NODE");

    return true;
}

int run_tests() {
    std::println("═══ Issue #126 verification tests ═══\n");
    test_should_relower();
    test_fnv1a_64();
    test_compute_dependencies();
    test_try_extract_define();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_126_detail

int aura_issue_126_run() { return aura_issue_126_detail::run_tests(); }

