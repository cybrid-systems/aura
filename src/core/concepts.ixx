// src/core/concepts.ixx — Issue #501: Aura core Concepts
// (foundation slice, scope-limited close).
//
// The Aura codebase has many template / generic functions
// that operate on AST nodes, mutations, allocators, and
// queries. Currently they use tag dispatch + manual
// if constexpr / overload resolution — no compile-time
// constraint on the actual types. This slice ships the
// FOUNDATION: the concept declarations + a minimal test.
//
// What's in here:
//   - NodeHandle: anything usable as an AST node id
//     (integral, strong typedef).
//   - ASTContainer: anything that exposes get(id) /
//     children(id) / tag(id).
//   - Mutator: anything that applies a mutation and
//     returns AuraResult.
//   - ArenaAllocator: anything that allocates + frees
//     bytes (the lowest layer of the stack).
//   - Queryable: anything that runs a query against an
//     AST and returns AuraResult.
//
// What's NOT in here (3 follow-ups tracked):
//   1. Apply ASTContainer to query_impl.cpp + query.ixx
//      (replace existing templates' implicit constraints
//       with the explicit concept).
//   2. Apply Mutator to mutation.ixx strategy classes
//      (cleaner dispatch + better error messages).
//   3. Apply ArenaAllocator to arena.ixx's internal
//      polymorphic allocator.
//
// Note on design:
// All concepts take EXPLICIT template parameters (no
// `auto` placeholders inside requires-clauses). GCC 16
// has edge-case bugs with placeholders inside concept
// requires-expressions in some forms, and the explicit
// form is just as readable for callers.

module;

#include <concepts>
#include <cstddef>
#include <ranges>
#include <type_traits>

export module aura.core.concepts;

import std;
import aura.core.error;

namespace aura::core {

// ── NodeHandle ────────────────────────────────────────────
//
// Anything that can be used as a FlatAST NodeId. We allow:
//   - integral types (uint32_t, uint64_t, etc. — the
//     current NodeId is std::uint32_t)
//   - any type with `.value()` returning an integral
//     (strong typedefs like a future NodeId wrapper)
//
// We DON'T constrain to a specific width because callers
// may pass typed NodeIds from different sub-systems (e.g.,
// a CursorId that's uint64_t). The constraint is on the
// SHAPE, not the size.
export template <typename T>
concept NodeHandle = std::integral<T> || requires(T t) {
    { t.value() } -> std::integral;
};

// ── ASTContainer ──────────────────────────────────────────
//
// Anything that exposes:
//   - get(id) → NodeView-like (returns view with tag +
//     children span)
//   - children(id) → a std::ranges::view
//   - tag(id) → returns Tag
//
// Today this is satisfied by FlatAST in aura.core.ast.
// We use ranges::view (not just ranges::range) so callers
// can pipe through ranges::views::filter / transform
// without copying.
//
// Two-parameter form: ASTContainer<C, Id> where C is the
// container type and Id is the node handle type. The
// one-parameter form ASTContainer<C> defaults Id to
// std::uint32_t (the current NodeId).
export template <typename C, typename Id = std::uint32_t>
concept ASTContainer = NodeHandle<Id> && requires(C& ast, Id id) {
    { ast.get(id) };
    { ast.children(id) } -> std::ranges::view;
    { ast.tag(id) };
};

// ── Mutator ───────────────────────────────────────────────
//
// A strategy class that applies a mutation to an AST and
// returns AuraResult<NodeHandle>. Used by mutation.ixx
// strategy dispatch (replace vs splice vs rename).
//
// Three-parameter form: Mutator<M, C, Id> where M is the
// mutator strategy type, C is the AST container type,
// and Id is the node handle type. One-parameter form
// Mutator<M> defaults C/AST to void and Id to
// std::uint32_t.
export template <typename M, typename C, typename Id = std::uint32_t>
concept Mutator = NodeHandle<Id> && requires(M m, C& ast, Id id) {
    { m.apply(ast, id) } -> std::same_as<AuraResult<Id>>;
};

// ── ArenaAllocator ────────────────────────────────────────
//
// Anything that allocates bytes. The lowest layer of the
// stack — every higher layer (AST, mutation, query)
// depends on this.
//
// Today this is satisfied by std::pmr::memory_resource +
// std::pmr::polymorphic_allocator + the AuraTypeArena.
// We don't pin to std::pmr::memory_resource specifically
// because callers may want to plug in a stack allocator,
// arena with defrag, or jemalloc-backed resource.
export template <typename A>
concept ArenaAllocator = requires(A& a, std::size_t bytes, void* p) {
    { a.allocate(bytes) } -> std::convertible_to<void*>;
    { a.deallocate(p, bytes) } noexcept;
};

// ── Queryable ─────────────────────────────────────────────
//
// A query strategy that runs against an AST and returns
// AuraResult<vector<NodeHandle>>. Used by query.ixx's
// dispatcher.
//
// Three-parameter form: Queryable<Q, C, Id> where Q is
// the query type, C is the AST container type, Id is
// the node handle type.
export template <typename Q, typename C, typename Id = std::uint32_t>
concept Queryable = NodeHandle<Id> && requires(Q q, C& ast) {
    { q.find_calls(ast, /*Symbol*/{}) }
        -> std::same_as<AuraResult<std::vector<Id>>>;
};

} // namespace aura::core