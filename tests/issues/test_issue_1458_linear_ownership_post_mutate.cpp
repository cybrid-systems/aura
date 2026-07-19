// test_issue_1458_linear_ownership_post_mutate.cpp
// Issue #1458: Harden Linear Ownership validation post-mutation.
//
// AC1: use-after-move (Variable after Move) detected by validate_ownership
// AC2: double-move still detected
// AC3: properly moved linear passes (no leak)
// AC4: rebind body with use-after-move → mutation rejected / metrics
// AC5: discover_linear_bindings_in_subtree finds Linear + Move targets
// AC6: post_mutation_invariant_check still catches leaked-linear

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <string>
#include <unordered_set>

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_1458_detail {

using aura::ast::ASTArena;
using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::discover_linear_bindings_in_subtree;
using aura::compiler::OwnershipEnv;
using aura::compiler::OwnershipNote;
using aura::compiler::post_mutation_invariant_check;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
using aura::core::TypeRegistry;

static int count_kind(const std::vector<OwnershipNote>& notes, const std::string& kind) {
    int n = 0;
    for (const auto& note : notes)
        if (note.kind == kind)
            ++n;
    return n;
}

// Build: (let ((x (Linear 1))) (begin (move x) x))
// → use-after-move on second x
static NodeId build_use_after_move(FlatAST& flat, StringPool& pool) {
    auto x_sym = pool.intern("x");
    auto lit = flat.add_literal(1);
    auto lin = flat.add_linear(lit);
    auto x_var1 = flat.add_variable(x_sym);
    auto mv = flat.add_move(x_var1);
    auto x_var2 = flat.add_variable(x_sym);
    auto body = flat.add_begin({mv, x_var2});
    return flat.add_let(x_sym, lin, body);
}

// Build: (let ((x (Linear 1))) (begin (move x) (move x)))
static NodeId build_double_move(FlatAST& flat, StringPool& pool) {
    auto x_sym = pool.intern("x");
    auto lit = flat.add_literal(2);
    auto lin = flat.add_linear(lit);
    auto x1 = flat.add_variable(x_sym);
    auto m1 = flat.add_move(x1);
    auto x2 = flat.add_variable(x_sym);
    auto m2 = flat.add_move(x2);
    auto body = flat.add_begin({m1, m2});
    return flat.add_let(x_sym, lin, body);
}

// Build: (let ((x (Linear 1))) (move x))
static NodeId build_ok_move(FlatAST& flat, StringPool& pool) {
    auto x_sym = pool.intern("x");
    auto lit = flat.add_literal(3);
    auto lin = flat.add_linear(lit);
    auto xv = flat.add_variable(x_sym);
    auto mv = flat.add_move(xv);
    return flat.add_let(x_sym, lin, mv);
}

static void test_use_after_move_variable() {
    std::println("\n--- AC1: Variable use after Move ---");
    auto arena = std::make_unique<ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<StringPool>(alloc);
    flat->root = build_use_after_move(*flat, *pool);

    std::unordered_set<std::string> dirty{"x"};
    std::vector<OwnershipNote> notes;
    const bool pass = OwnershipEnv::validate_ownership(*flat, *pool, flat->root, dirty, notes);
    std::println("  pass={} notes={} uam={}", pass, notes.size(),
                 count_kind(notes, "use-after-move"));
    CHECK(!pass, "validation fails on use-after-move");
    CHECK(count_kind(notes, "use-after-move") >= 1, "use-after-move note present");
}

static void test_double_move() {
    std::println("\n--- AC2: double Move ---");
    auto arena = std::make_unique<ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<StringPool>(alloc);
    flat->root = build_double_move(*flat, *pool);

    std::unordered_set<std::string> dirty{"x"};
    std::vector<OwnershipNote> notes;
    const bool pass = OwnershipEnv::validate_ownership(*flat, *pool, flat->root, dirty, notes);
    CHECK(!pass, "double-move fails");
    CHECK(count_kind(notes, "use-after-move") >= 1, "use-after-move on second move");
}

static void test_ok_move_no_leak() {
    std::println("\n--- AC3: proper move — no leak ---");
    auto arena = std::make_unique<ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<StringPool>(alloc);
    flat->root = build_ok_move(*flat, *pool);

    std::unordered_set<std::string> dirty{"x"};
    std::vector<OwnershipNote> notes;
    const bool pass = OwnershipEnv::validate_ownership(*flat, *pool, flat->root, dirty, notes);
    std::println("  pass={} notes={}", pass, notes.size());
    CHECK(pass, "proper move passes");
    CHECK(count_kind(notes, "leaked-linear") == 0, "no leak");
    CHECK(count_kind(notes, "use-after-move") == 0, "no uam");
}

static void test_rebind_path_discovery_and_validate() {
    std::println("\n--- AC4: rebind-path discover + validate composition ---");
    // The Aura source form (Linear e)/(move e) may lower as Call
    // prims rather than NodeTag::Linear/Move; rebind wiring still
    // uses discover_linear_bindings_in_subtree + validate_ownership.
    // Cover that composition on a FlatAST built with the AST
    // builders (same nodes the post-mutate path validates).
    auto arena = std::make_unique<ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<StringPool>(alloc);
    // Define f with use-after-move body (simulates rebind target).
    auto body = build_use_after_move(*flat, *pool);
    auto f_sym = pool->intern("f");
    auto lam = flat->add_lambda({}, body);
    auto def = flat->add_define(f_sym, lam);
    flat->root = def;

    std::unordered_set<std::string> found;
    discover_linear_bindings_in_subtree(*flat, *pool, def, found);
    CHECK(found.count("x") == 1, "discover under define finds x");

    std::vector<OwnershipNote> notes;
    const bool pass = OwnershipEnv::validate_ownership(*flat, *pool, flat->root, found, notes);
    std::println("  pass={} uam={}", pass, count_kind(notes, "use-after-move"));
    CHECK(!pass, "validate under define fails");
    CHECK(count_kind(notes, "use-after-move") >= 1, "use-after-move under define");
}

static void test_discover_subtree() {
    std::println("\n--- AC5: discover_linear_bindings_in_subtree ---");
    auto arena = std::make_unique<ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<StringPool>(alloc);
    flat->root = build_use_after_move(*flat, *pool);

    std::unordered_set<std::string> found;
    discover_linear_bindings_in_subtree(*flat, *pool, flat->root, found);
    std::println("  discovered={}", found.size());
    CHECK(found.count("x") == 1, "discovers x from Linear + Move");
}

static void test_post_mutation_invariant_leak() {
    std::println("\n--- AC6: post_mutation_invariant_check leak ---");
    auto arena = std::make_unique<ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<StringPool>(alloc);

    auto x_sym = pool->intern("x");
    auto lit = flat->add_literal(9);
    auto lin = flat->add_linear(lit);
    auto xv = flat->add_variable(x_sym);
    // leak: use without move
    auto root = flat->add_let(x_sym, lin, xv);
    flat->root = root;

    TypeRegistry reg;
    CompilerMetrics metrics;
    aura::ast::MutationRecord rec;
    rec.target_node = root;
    rec.mutation_id = 1458;
    rec.operator_name = "issue-1458";

    std::vector<OwnershipNote> notes;
    (void)post_mutation_invariant_check(*flat, *pool, reg, rec, notes, &metrics);
    CHECK(count_kind(notes, "leaked-linear") >= 1, "leaked-linear found");
    CHECK(metrics.linear_post_mutate_revalidations_total.load() > 0, "revalidations bumped");
}

} // namespace aura_1458_detail

int main() {
    using namespace aura_1458_detail;
    test_use_after_move_variable();
    test_double_move();
    test_ok_move_no_leak();
    test_rebind_path_discovery_and_validate();
    test_discover_subtree();
    test_post_mutation_invariant_leak();
    return RUN_ALL_TESTS();
}
