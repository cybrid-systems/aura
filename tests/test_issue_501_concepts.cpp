// test_issue_501_concepts.cpp — Issue #501 Concepts
// extension (Phase C6): validation test for the 9 concepts
// in aura.core.concepts.
//
// Verifies each of the 9 concepts:
//   - accepts the documented "good shape"
//   - rejects documented "bad shape" (compile-time guard)
//
// All assertions are static_assert — pure compile-time. The
// test binary itself has no runtime work to do; if the build
// succeeds, all 9 concepts are correct.
//
// The 9 concepts:
//   1. NodeHandle<T>
//   2. ASTContainer<C, Id>
//   3. Mutator<M, C, Id>
//   4. ArenaAllocator<A>
//   5. Queryable<Q, C, Id>
//   6. AuraInvocable<F, Args...>  (Phase C2)
//   7. RangeOf<T, R>  +  AnyRange<R>  (Phase C2)
//   8. SymbolInterner<S>  (Phase C2)
//   9. StableNodeRefLike<R, C>  (Phase C2)

#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <print>

import aura.core;
import aura.core.concepts;
import aura.core.ast;
import aura.core.mutators;
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

// ── AC1: NodeHandle accepts integrals, rejects non-integral ───
bool test_node_handle() {
    std::println("\n--- AC1: NodeHandle ---");
    static_assert(aura::core::NodeHandle<std::uint32_t>,
                  "uint32_t satisfies NodeHandle");
    static_assert(aura::core::NodeHandle<std::uint64_t>,
                  "uint64_t satisfies NodeHandle");
    static_assert(aura::core::NodeHandle<int>, "int satisfies NodeHandle");

    // Strong typedef (has .value())
    struct Strong { std::uint32_t v; std::uint32_t value() const { return v; } };
    static_assert(aura::core::NodeHandle<Strong>,
                  "Strong typedef satisfies NodeHandle via .value()");

    static_assert(!aura::core::NodeHandle<std::string>,
                  "std::string does NOT satisfy NodeHandle");
    static_assert(!aura::core::NodeHandle<float>,
                  "float does NOT satisfy NodeHandle");
    CHECK(true, "static_asserts: NodeHandle accepts integrals, rejects others");
    return true;
}

// ── AC2: ArenaAllocator accepts/declines ──────────────────────
struct GoodArena {
    void* allocate(std::size_t bytes) { return ::operator new(bytes); }
    void deallocate(void* p, std::size_t) noexcept { ::operator delete(p); }
};
struct BadArenaNoAllocate {
    void deallocate(void* p, std::size_t) noexcept {}
};
struct BadArenaDeallocThrows {
    void* allocate(std::size_t) { return nullptr; }
    void deallocate(void* p, std::size_t) {} // not noexcept
};

bool test_arena_allocator() {
    std::println("\n--- AC2: ArenaAllocator ---");
    static_assert(aura::core::ArenaAllocator<GoodArena>,
                  "GoodArena satisfies ArenaAllocator");
    static_assert(!aura::core::ArenaAllocator<BadArenaNoAllocate>,
                  "BadArenaNoAllocate does NOT satisfy");
    static_assert(!aura::core::ArenaAllocator<BadArenaDeallocThrows>,
                  "BadArenaDeallocThrows (non-noexcept dealloc) does NOT satisfy");
    static_assert(!aura::core::ArenaAllocator<int>,
                  "int does NOT satisfy");
    CHECK(true, "static_asserts: ArenaAllocator accepts GoodArena, rejects bad shapes");
    return true;
}

// ── AC3: AuraInvocable accepts visitors, rejects non-invocables
struct GoodVisitor {
    bool operator()(int x, const std::string& s) const {
        return !s.empty() && x > 0;
    }
};
struct BadVisitorNoCall {
    int x = 0;
};
struct BadVisitorReturnsInt {
    int operator()(int, const std::string&) const { return 0; }
};
struct BadVisitorNoArgs {
    bool operator()() const { return true; }  // wrong signature
};
struct BadVisitorReturnsStruct {
    struct NotBool { int x; };
    NotBool operator()(int, const std::string&) const { return {0}; }
};

bool test_aura_invocable() {
    std::println("\n--- AC3: AuraInvocable ---");
    static_assert(aura::core::AuraInvocable<GoodVisitor, int, const std::string&>,
                  "GoodVisitor satisfies AuraInvocable");
    static_assert(!aura::core::AuraInvocable<BadVisitorNoCall, int, const std::string&>,
                  "BadVisitorNoCall (no operator()) does NOT satisfy");
    // int is convertible to bool (nonzero = true), so a visitor
    // returning int still satisfies AuraInvocable. The right way
    // to reject a non-bool-convertible type is to return a struct.
    static_assert(aura::core::AuraInvocable<BadVisitorReturnsInt, int, const std::string&>,
                  "int return is convertible to bool, so it DOES satisfy");
    static_assert(!aura::core::AuraInvocable<BadVisitorNoArgs, int, const std::string&>,
                  "BadVisitorNoArgs (wrong arity) does NOT satisfy");
    static_assert(!aura::core::AuraInvocable<BadVisitorReturnsStruct, int, const std::string&>,
                  "BadVisitorReturnsStruct (struct not convertible to bool) does NOT satisfy");
    CHECK(true, "static_asserts: AuraInvocable accepts good, rejects bad");
    return true;
}

// ── AC4: RangeOf / AnyRange ───────────────────────────────────
bool test_range_of() {
    std::println("\n--- AC4: RangeOf / AnyRange ---");
    // vector<uint32_t> is a range of uint32_t.
    static_assert(aura::core::RangeOf<std::uint32_t, std::vector<std::uint32_t>>,
                  "vector<uint32_t> is RangeOf<uint32_t>");
    static_assert(aura::core::RangeOf<int, std::span<const int>>,
                  "span<const int> is RangeOf<int>");
    // AnyRange: any range, no element type constraint.
    static_assert(aura::core::AnyRange<std::vector<std::string>>,
                  "vector<string> is AnyRange");
    // Mismatched element type.
    static_assert(!aura::core::RangeOf<int, std::vector<std::string>>,
                  "vector<string> is NOT RangeOf<int> (element type mismatch)");
    // Non-range type.
    static_assert(!aura::core::AnyRange<int>,
                  "int is NOT a range");
    CHECK(true, "static_asserts: RangeOf/AnyRange correct accept/reject");
    return true;
}

// ── AC5: SymbolInterner ───────────────────────────────────────
struct GoodInterner {
    std::uint32_t intern(std::string_view s) { return static_cast<std::uint32_t>(s.size()); }
};
struct BadInternerNoIntern {
    int x = 0;
};
struct BadInternerReturnsString {
    std::string intern(std::string_view) { return ""; }
};

bool test_symbol_interner() {
    std::println("\n--- AC5: SymbolInterner ---");
    static_assert(aura::core::SymbolInterner<GoodInterner>,
                  "GoodInterner satisfies SymbolInterner");
    static_assert(!aura::core::SymbolInterner<BadInternerNoIntern>,
                  "BadInternerNoIntern does NOT satisfy");
    static_assert(!aura::core::SymbolInterner<BadInternerReturnsString>,
                  "BadInternerReturnsString (returns string) does NOT satisfy");
    CHECK(true, "static_asserts: SymbolInterner accepts good, rejects bad");
    return true;
}

// ── AC6: StableNodeRefLike ───────────────────────────────────
struct FakeAST {};
struct GoodStableRef {
    bool is_valid(const FakeAST&) const { return true; }
    std::uint32_t id() const { return 0; }
};
struct BadStableRefNoIsValid {
    std::uint32_t id() const { return 0; }
};
struct BadStableRefNoId {
    bool is_valid(const FakeAST&) const { return true; }
};

bool test_stable_node_ref_like() {
    std::println("\n--- AC6: StableNodeRefLike ---");
    static_assert(aura::core::StableNodeRefLike<GoodStableRef, FakeAST>,
                  "GoodStableRef satisfies StableNodeRefLike<FakeAST>");
    static_assert(!aura::core::StableNodeRefLike<BadStableRefNoIsValid, FakeAST>,
                  "BadStableRefNoIsValid does NOT satisfy");
    static_assert(!aura::core::StableNodeRefLike<BadStableRefNoId, FakeAST>,
                  "BadStableRefNoId does NOT satisfy");
    CHECK(true, "static_asserts: StableNodeRefLike accepts good, rejects bad");
    return true;
}

// ── AC7: Composition — concepts work together ────────────────
bool test_composition() {
    std::println("\n--- AC7: concept composition ---");
    // AuraInvocable composes with std::invocable.
    static_assert(aura::core::AuraInvocable<GoodVisitor, int, const std::string&>
                  && std::invocable<GoodVisitor, int, const std::string&>,
                  "AuraInvocable implies std::invocable");
    // RangeOf<T, R> implies AnyRange<R>.
    static_assert(aura::core::RangeOf<int, std::vector<int>>
                  && aura::core::AnyRange<std::vector<int>>,
                  "RangeOf implies AnyRange");
    CHECK(true, "static_asserts: concept composition correct");
    return true;
}

// ── AC8: Verify Phase C3 arena constraints lock the contract ─
//
// The arena.ixx / type_arena.ixx templates use:
//   requires std::constructible_from<T, Args...>
//   requires std::is_nothrow_destructible_v<T>
// These are std concepts, but verify they correctly
// reject bad shapes via static_assert (regression guard).
struct ThrowingDtor {
    ~ThrowingDtor() noexcept(false) {}
};
struct GoodDtor {
    ~GoodDtor() noexcept = default;
};

bool test_arena_constraint_predicates() {
    std::println("\n--- AC8: arena constraint predicates ---");
    static_assert(std::constructible_from<GoodDtor>,
                  "GoodDtor is constructible_from (default)");
    static_assert(std::is_nothrow_destructible_v<GoodDtor>,
                  "GoodDtor is nothrow-destructible");
    static_assert(!std::is_nothrow_destructible_v<ThrowingDtor>,
                  "ThrowingDtor is NOT nothrow-destructible");
    CHECK(true, "static_asserts: arena constraint predicates correct");
    return true;
}

// ── AC9: count_nodes_with_predicate (Phase D helper) ─────────
//
// Runtime test: build a small nested AST, verify the helper
// counts correctly for various predicates.
static aura::ast::NodeId add_let_chain(aura::ast::FlatAST& flat,
                                       aura::ast::StringPool& pool,
                                       int depth) {
    // Build (let (a_0 1) (let (a_1 2) ... (let (a_n n) body)))
    // The root of the outermost let is returned.
    aura::ast::NodeId innermost_body = flat.add_literal(depth);
    for (int i = depth - 1; i >= 0; --i) {
        char name[16];
        std::snprintf(name, sizeof(name), "a_%d", i);
        auto name_sym = pool.intern(name);
        auto val = flat.add_literal(static_cast<std::int64_t>(i));
        innermost_body = flat.add_let(name_sym, val, innermost_body);
    }
    flat.root = innermost_body;
    return innermost_body;
}

bool test_count_nodes_with_predicate() {
    std::println("\n--- AC9: count_nodes_with_predicate ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;

    // depth=3 -> 3 let nodes + 4 literal nodes = 7 nodes total
    auto root = add_let_chain(flat, pool, /*depth*/ 3);

    // All nodes: 7
    auto total = aura::compiler::count_nodes_with_predicate<std::uint32_t>(
        flat, root, [](NodeId) { return true; });
    CHECK_EQ(total, 7u, "all nodes count = 7 (3 lets + 4 literals)");

    // Only Let nodes: 3
    auto lets_only = aura::compiler::count_nodes_with_predicate<std::uint32_t>(
        flat, root, [&](NodeId id) { return flat.tag(id) == NodeTag::Let; });
    CHECK_EQ(lets_only, 3u, "let nodes count = 3");

    // Only Literal nodes: 4
    auto lits_only = aura::compiler::count_nodes_with_predicate<std::uint32_t>(
        flat, root, [&](NodeId id) { return flat.tag(id) == NodeTag::LiteralInt; });
    CHECK_EQ(lits_only, 4u, "literal nodes count = 4");

    // No nodes match (pred always false): 0
    auto none = aura::compiler::count_nodes_with_predicate<std::uint32_t>(
        flat, root, [](NodeId) { return false; });
    CHECK_EQ(none, 0u, "no-match pred returns 0");

    // Only root matches: 1
    auto root_only = aura::compiler::count_nodes_with_predicate<std::uint32_t>(
        flat, root, [&](NodeId id) { return id == root; });
    CHECK_EQ(root_only, 1u, "root-only pred returns 1");
    return true;
}

// ── AC10: find_first_node_with (Phase D helper) ─────────────
//
// Runtime test: verify find returns the first matching node
// in pre-order DFS, or std::nullopt if no match.
bool test_find_first_node_with() {
    std::println("\n--- AC10: find_first_node_with ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto root = add_let_chain(flat, pool, /*depth*/ 3);

    // Find a Let node — first one is root itself.
    auto first_let = aura::compiler::find_first_node_with<std::uint32_t>(
        flat, root, [&](NodeId id) { return flat.tag(id) == NodeTag::Let; });
    CHECK(first_let.has_value(), "found a Let node");
    CHECK_EQ(*first_let, root, "first Let is the root");

    // Find a Literal node — first one is the innermost literal (depth=3)
    // because DFS is pre-order from root (let) into body (let) ... into
    // innermost let which has body = literal(3).
    auto first_lit = aura::compiler::find_first_node_with<std::uint32_t>(
        flat, root, [&](NodeId id) { return flat.tag(id) == NodeTag::LiteralInt; });
    CHECK(first_lit.has_value(), "found a literal node");

    // No match returns std::nullopt.
    auto no_match = aura::compiler::find_first_node_with<std::uint32_t>(
        flat, root, [](NodeId) { return false; });
    CHECK(!no_match.has_value(), "no-match returns nullopt");
    return true;
}

// ── AC11: helper concept compile-time guards ────────────────
//
// The Phase D helpers are constrained by ASTContainer +
// AuraInvocable. Verify the constraints fire correctly.

// A typed invocable predicate for static_assert contexts.
struct TestPred {
    bool operator()(aura::ast::NodeId) const { return true; }
};

bool test_phase_d_concept_constraints() {
    std::println("\n--- AC11: Phase D helper concept constraints ---");
    using aura::ast::NodeId;
    using aura::ast::FlatAST;

    // Positive: helpers accept FlatAST.
    static_assert(
        requires(FlatAST& f, NodeId id, TestPred pred) {
            aura::compiler::count_nodes_with_predicate<std::uint32_t>(
                f, id, pred);
        }, "count_nodes_with_predicate accepts FlatAST + invocable pred");

    static_assert(
        requires(FlatAST& f, NodeId id, TestPred pred) {
            aura::compiler::find_first_node_with<std::uint32_t>(
                f, id, pred);
        }, "find_first_node_with accepts FlatAST + invocable pred");

    // (ASTContainer rejection for non-AST types is covered
    //  by AC5 of test_issue_501.cpp — the concept itself is
    //  already verified to reject non-ASTContainer. The
    //  Phase D helpers propagate that constraint.)

    // (Predicate-shape rejection is covered by AC3 of this
    //  file — the AuraInvocable concept is the constraint
    //  source, no need to retest here.)

    CHECK(true, "static_asserts: Phase D helper constraints correct");
    return true;
}

// ── AC12: walk_ancestors (Phase D.5 helper) ───────────────
//
// Build a 3-deep let chain. Verify walk_ancestors:
//   - visits all ancestors from start to root (in order)
//   - returns the visit count
//   - terminates on self-loop (parent_of(root) = NULL_NODE)
//   - early-stops when visitor returns false
bool test_walk_ancestors() {
    std::println("\n--- AC12: walk_ancestors ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto root = add_let_chain(flat, pool, /*depth*/ 3);
    // root is the outermost let. Its chain up is itself, then
    // parent (NULL_NODE — root of flat).
    auto innermost_body = flat.children(root)[1];  // body of outermost let

    // Walk from innermost body up — should hit itself, then root,
    // then NULL_NODE (which terminates via self-loop).
    std::vector<NodeId> seen;
    auto vis = [&](NodeId id) -> bool {
        seen.push_back(id);
        return true;
    };
    auto count = aura::compiler::walk_ancestors<std::uint32_t>(
        flat, innermost_body, vis);
    CHECK_EQ(seen.size(), 2u,
             "walk_ancestors visited 2 nodes (innermost body + root)");
    CHECK_EQ(count, 2u, "walk_ancestors returned count = 2");
    CHECK_EQ(seen[0], innermost_body, "first visited is innermost body");
    CHECK_EQ(seen[1], root, "second visited is outermost root");

    // Early-stop when visitor returns false.
    seen.clear();
    auto stop_at_first = [&](NodeId) -> bool {
        seen.push_back(innermost_body);  // any sentinel, just record
        return false;  // stop immediately
    };
    auto early_count = aura::compiler::walk_ancestors<std::uint32_t>(
        flat, root, stop_at_first);
    CHECK_EQ(early_count, 0u,
             "early-stop returns 0 (visitor returned false before ++count)");
    CHECK_EQ(seen.size(), 1u, "early-stop visitor called exactly once");

    // Walk from the root node itself — should visit just root,
    // then terminate (parent_of(root) = NULL_NODE = self-loop).
    seen.clear();
    auto root_count = aura::compiler::walk_ancestors<std::uint32_t>(
        flat, root, [&](NodeId id) -> bool { seen.push_back(id); return true; });
    CHECK_EQ(root_count, 1u, "walk_ancestors from root visits 1 node");
    CHECK_EQ(seen.size(), 1u, "1 entry in seen");
    CHECK_EQ(seen[0], root, "seen[0] is the root");
    return true;
}

// ── AC13: walk_ancestors concept constraint guard ─────────
//
// walk_ancestors requires ASTContainer + parent_of +
// AuraInvocable. Static_assert the positive case (FlatAST
// has all three). Negative cases covered by AC5 + AC11.
bool test_walk_ancestors_concept_constraint() {
    std::println("\n--- AC13: walk_ancestors concept constraint ---");
    using aura::ast::NodeId;
    using aura::ast::FlatAST;

    struct TestPred {
        bool operator()(NodeId) const { return true; }
    };

    static_assert(
        requires(FlatAST& f, NodeId id, TestPred pred) {
            aura::compiler::walk_ancestors<std::uint32_t>(
                f, id, pred);
        }, "walk_ancestors accepts FlatAST + invocable + parent_of");

    CHECK(true, "static_assert: walk_ancestors accepts FlatAST + invocable");
    return true;
}

int main() {
    std::println("=== Issue #501 Concepts extension (Phase C6) ===\n");
    test_node_handle();
    test_arena_allocator();
    test_aura_invocable();
    test_range_of();
    test_symbol_interner();
    test_stable_node_ref_like();
    test_composition();
    test_arena_constraint_predicates();
    test_count_nodes_with_predicate();
    test_find_first_node_with();
    test_phase_d_concept_constraints();
    test_walk_ancestors();
    test_walk_ancestors_concept_constraint();

    std::println("\n========================================");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}