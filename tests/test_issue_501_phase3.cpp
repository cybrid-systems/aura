// test_issue_501_phase3.cpp — Issue #501 Phase 3:
// Apply ASTContainer concept to query (scope-limited close).
//
// Verifies the ASTContainer concept relaxation
// (std::ranges::view -> std::ranges::range) makes FlatAST
// satisfy it, and that the new walk_children<> template
// helper in aura.compiler.query (ASTContainer-constrained)
// correctly traverses FlatAST children with a visitor.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <print>

import aura.core.mutators;
import aura.core.ast;
import aura.core.concepts;
import aura.compiler.query;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println("  PASS: {}", msg); } \
    else      { ++g_failed; std::println("  FAIL: {}", msg); } \
} while (0)

#define CHECK_EQ(a, b, msg) do { \
    auto _a = (a); auto _b = (b); \
    if (_a == _b) { ++g_passed; std::println("  PASS: {}  ({} == {})", msg, _a, _b); } \
    else          { ++g_failed; std::println("  FAIL: {}  ({} != {})", msg, _a, _b); } \
} while (0)

// Helper: build a 2-child let node.
static aura::ast::NodeId make_let_2(aura::ast::FlatAST& flat,
                                    aura::ast::StringPool& pool,
                                    const char* name,
                                    std::int64_t val) {
    auto name_sym = pool.intern(name);
    auto val_node = flat.add_literal(val);
    auto id = flat.add_let(name_sym, val_node, aura::ast::NULL_NODE);
    flat.root = id;
    return id;
}

// ── AC1: ASTContainer relaxation — span qualifies ─────────
//
// Phase 3 relaxed the concept from std::ranges::view to
// std::ranges::range. Verify FlatAST now satisfies it
// (FlatAST::children() returns std::span<const NodeId>).
bool test_flatast_satisfies_astcontainer() {
    std::println("\n--- AC1: FlatAST satisfies ASTContainer ---");
    using aura::ast::FlatAST;
    static_assert(aura::core::ASTContainer<FlatAST>,
                  "FlatAST must satisfy ASTContainer after Phase 3 relaxation");
    CHECK(true, "static_assert: FlatAST satisfies ASTContainer<FlatAST>");
    return true;
}

// ── AC2: ASTContainer relaxation rejects non-range shapes ─
//
// The relaxed concept still rejects children() returning a
// non-range type (e.g., int). Compile-time guard.
struct RangeAcceptingAST {
    auto get(std::uint32_t) const { return 0; }
    auto children(std::uint32_t) { return std::vector<std::uint32_t>{}; }
    auto tag(std::uint32_t) const { return 0; }
};
struct NonRangeAST {
    auto get(std::uint32_t) const { return 0; }
    int children(std::uint32_t) { return 0; }  // NOT a range
    auto tag(std::uint32_t) const { return 0; }
};

bool test_relaxed_concept_accepts_span_and_rejects_int() {
    std::println("\n--- AC2: relaxed concept accepts range, rejects non-range ---");
    static_assert(aura::core::ASTContainer<RangeAcceptingAST, std::uint32_t>,
                  "AST with vector children() now satisfies ASTContainer");
    static_assert(!aura::core::ASTContainer<NonRangeAST, std::uint32_t>,
                  "AST with int children() still does NOT satisfy ASTContainer");
    CHECK(true, "static_asserts: vector OK, int rejected");
    return true;
}

// ── AC3: walk_children visits all children ────────────────
//
// The new query.ixx helper `walk_children<Id, C, V>(ast,
// root, vis)` is constrained by ASTContainer. Verify it
// visits every child and returns the visit count.
bool test_walk_children_visits_all() {
    std::println("\n--- AC3: walk_children visits all children ---");
    using namespace aura::ast;
    using namespace aura::compiler;

    FlatAST flat;
    StringPool pool;
    auto let_id = make_let_2(flat, pool, "a", 1);

    // Simple visitor: count children.
    std::size_t count = 0;
    auto visitor = [&count](NodeId) { ++count; };

    auto visited = walk_children<std::uint32_t, FlatAST>(
        flat, let_id, visitor);
    CHECK_EQ(visited, 2u, "walk_children returns child count");
    CHECK_EQ(count, 2u, "visitor was called for each child");

    // Verify visitor saw the right children (by collecting them).
    std::vector<NodeId> seen;
    auto collector = [&seen](NodeId id) { seen.push_back(id); };
    walk_children<std::uint32_t, FlatAST>(flat, let_id, collector);
    CHECK_EQ(seen.size(), 2u, "collector saw 2 children");
    CHECK_EQ(seen[0], flat.children(let_id)[0],
             "seen[0] matches FlatAST::children[0]");
    CHECK_EQ(seen[1], flat.children(let_id)[1],
             "seen[1] matches FlatAST::children[1]");
    return true;
}

// ── AC4: walk_children handles 0-child nodes ──────────────
//
// Edge case: a node with no children (e.g., Variable).
// walk_children must visit 0 times and return 0.
bool test_walk_children_zero_children() {
    std::println("\n--- AC4: walk_children on 0-child node ---");
    using namespace aura::ast;
    using namespace aura::compiler;

    FlatAST flat;
    StringPool pool;
    auto var_id = flat.add_variable(pool.intern("x"));

    std::size_t count = 0;
    auto visitor = [&count](NodeId) { ++count; };
    auto visited = walk_children<std::uint32_t, FlatAST>(flat, var_id, visitor);
    CHECK_EQ(visited, 0u, "walk_children returns 0 for empty children");
    CHECK_EQ(count, 0u, "visitor never called for empty children");
    return true;
}

// ── AC5: walk_children is generic — works with any ASTContainer ─
//
// Compile-time check that walk_children accepts both
// FlatAST and a custom ASTContainer (RangeAcceptingAST).
// Runtime: only FlatAST is exercised; RangeAcceptingAST
// is verified via static_assert.
bool test_walk_children_generic() {
    std::println("\n--- AC5: walk_children generic over ASTContainer ---");
    using namespace aura::ast;
    using namespace aura::compiler;

    static_assert(
        requires(FlatAST& f, NodeId id) {
            walk_children<std::uint32_t, FlatAST>(f, id,
                [](NodeId) {});
        },
        "walk_children must accept FlatAST");

    static_assert(
        requires(RangeAcceptingAST& a, std::uint32_t id) {
            walk_children<std::uint32_t, RangeAcceptingAST>(a, id,
                [](std::uint32_t) {});
        },
        "walk_children must accept any ASTContainer (not just FlatAST)");

    CHECK(true, "static_asserts: walk_children accepts FlatAST and custom AST");
    return true;
}

// ── AC6: walk_children rejects non-ASTContainer ──────────
//
// Compile-time guard: a struct without children() must NOT
// satisfy walk_children's ASTContainer constraint.
struct NotAnAST {
    int x = 0;
};

bool test_walk_children_rejects_non_astcontainer() {
    std::println("\n--- AC6: walk_children rejects non-ASTContainer ---");
    using aura::ast::NodeId;
    // The cleanest test: verify that walk_children compiles for
    // FlatAST (which IS an ASTContainer) and rejects NotAnAST
    // (which is NOT an ASTContainer) by trying to call it.
    // For the negative case, we use std::is_invocable_v which
    // uses SFINAE to detect callability without hard errors.
    auto flatast_callable = [](aura::ast::FlatAST& f, NodeId id) {
        return aura::compiler::walk_children<std::uint32_t, aura::ast::FlatAST>(
            f, id, [](NodeId) {});
    };
    static_assert(
        std::is_invocable_v<decltype(flatast_callable),
                            aura::ast::FlatAST&, NodeId>,
        "walk_children must accept FlatAST");
    (void)flatast_callable;

    // The negative case: NotAnAST doesn't have children()/get()/tag()
    // so it doesn't satisfy ASTContainer. Verify directly via the
    // concept (not via walk_children instantiation, which would
    // hard-error inside concept machinery).
    static_assert(
        !aura::core::ASTContainer<NotAnAST, std::uint32_t>,
        "NotAnAST does NOT satisfy ASTContainer (no get/children/tag)");

    CHECK(true, "static_asserts: walk_children accepts FlatAST, NotAnAST fails ASTContainer");
    return true;
}

int main() {
    std::println("=== Issue #501 Phase 3 verification ===\n");
    std::println("AC #1: FlatAST satisfies ASTContainer");
    test_flatast_satisfies_astcontainer();
    std::println("\nAC #2: relaxed concept accepts range, rejects non-range");
    test_relaxed_concept_accepts_span_and_rejects_int();
    std::println("\nAC #3: walk_children visits all children");
    test_walk_children_visits_all();
    std::println("\nAC #4: walk_children handles 0-child nodes");
    test_walk_children_zero_children();
    std::println("\nAC #5: walk_children generic over ASTContainer");
    test_walk_children_generic();
    std::println("\nAC #6: walk_children rejects non-ASTContainer");
    test_walk_children_rejects_non_astcontainer();

    std::println("\n========================================");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}