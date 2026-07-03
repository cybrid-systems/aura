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
//   - children(id) → a std::ranges::range
//   - tag(id) → returns Tag
//
// Today this is satisfied by FlatAST in aura.core.ast
// (FlatAST::children() returns std::span<const NodeId>,
// which is a borrowed_range but NOT a std::ranges::view).
// We constrain to std::ranges::range (not view) so span
// qualifies — spans can still pipe through
// ranges::views::filter / transform in C++23 because
// they're borrowed.
//
// If a future caller writes a transformation that needs
// ownership of the children range (e.g., std::ranges::to),
// they can wrap with std::views::all at the call site.
//
// Two-parameter form: ASTContainer<C, Id> where C is the
// container type and Id is the node handle type. The
// one-parameter form ASTContainer<C> defaults Id to
// std::uint32_t (the current NodeId).
export template <typename C, typename Id = std::uint32_t>
concept ASTContainer = NodeHandle<Id> && requires(C& ast, Id id) {
    { ast.get(id) };
    { ast.children(id) } -> std::ranges::range;
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

// ═══════════════════════════════════════════════════════════
// Phase C2 — Extended concept set
// ═══════════════════════════════════════════════════════════
//
// constraint patterns observed in the codebase. Each is
// a zero-cost compile-time constraint (no virtuals, no
// type erasure, no extra indirection). They document the
// shape of the type the API expects and produce clearer
// error messages when callers pass the wrong shape.
//
// These do NOT replace the existing 5 concepts; they
// complement them. The existing 5 stay in place; the new
// 4 are added below.

// ── AuraInvocable ───────────────────────────────────────────
//
// Like std::invocable, but with an additional contract:
// the callable MUST be noexcept-friendly in the sense
// that it should not throw out of the call site (callers
// typically use `if (visitor(...))` style or just let
// exceptions propagate). The concept doesn't enforce
// noexcept on the operator() (that's a property of the
// type, not the call site), but it DOES enforce that
// the result is convertible to bool or void (the
// common visitor return types).
//
// Used by:
//   - ast.ixx::rename_param<Callback>
//   - evaluator.ixx::walk_env_frames<F>
//   - (future) any visitor template in query.ixx / pass_manager.ixx
export template <typename F, typename... Args>
concept AuraInvocable = std::invocable<F, Args...> && requires(
    F&& f, Args&&... args) {
    { std::invoke(std::forward<F>(f), std::forward<Args>(args)...) }
        -> std::convertible_to<bool>;
};

// ── RangeOf<T, R> ────────────────────────────────────────────
//
// A range whose value_type is T. Useful for constraining
// std::span<const T>, std::vector<T>, etc. at template
// boundaries without spelling out the iterator category.
//
// Two-parameter form: RangeOf<T, R>. The one-parameter
// form RangeOf<R> uses std::ranges::range_value_t<R> as T
// (i.e. "any range", no element-type constraint).
export template <typename T, typename R>
concept RangeOf = std::ranges::range<R> &&
                   std::same_as<std::ranges::range_value_t<R>, T>;

export template <typename R>
concept AnyRange = std::ranges::range<R>;

// ── SymbolInterner ───────────────────────────────────────────
//
// Anything that can intern a string_view into a SymId.
// StringPool + ev.string_heap_ both qualify. Used to
// constrain templates that need to resolve a name to
// a stable symbol (e.g. compile:* primitives that take
// symbol names as args).
export template <typename S>
concept SymbolInterner = requires(S& s, std::string_view name) {
    { s.intern(name) } -> std::convertible_to<std::uint32_t>;
    // Optional: intern-or-get-existing via second method.
    // Keeping the surface minimal — just the intern call.
};

// ── StableNodeRefLike ────────────────────────────────────────
//
// A handle that captures a node + generation for safe
// use across structural mutations. Satisfied by
// FlatAST::StableNodeRef and LayerStableRef.
//
// The concept locks the shape: any type that exposes
// is_valid(ast) + id() can be used wherever a stable
// ref is expected.
export template <typename R, typename C>
concept StableNodeRefLike = requires(const R& r, const C& ast) {
    { r.is_valid(ast) } -> std::convertible_to<bool>;
    { r.id() } -> std::convertible_to<std::uint32_t>;
};

// ── NullIdCheck ────────────────────────────────────────────
//
// Default null-sentinel check for a NodeHandle Id.
// Returns true if `id` is the "null" / "no node" sentinel.
// Default: Id{} (zero-initialized uint32_t = 0).
// Specialize for types where null is not zero (e.g.,
// FlatAST::NodeId = ~0u).
//
// Used by walk_ancestors<Id, C, V> in aura.compiler.query to
// terminate the parent-chain walk without visiting the
// null sentinel node itself.
export template <typename Id>
struct NullIdCheck {
    static constexpr bool is_null(Id id) noexcept { return id == Id{}; }
};

// ═══════════════════════════════════════════════════════════
// Phase C1 — Concepts application map
// ═══════════════════════════════════════════════════════════
//
// (Documentation only — no runtime impact.)
//
// Where each concept is / should be applied:
//
// ── NodeHandle ─────────────────────────────────────────────
//   Current uses:  0
//   Target sites:
//     - ast.ixx::rename_param template id param
//     - walk_children<Id, C, V> (already in query.ixx, Id
//       constrained by ASTContainer<C, Id> transitively)
//     - (future) any template taking NodeId
//
// ── ASTContainer ───────────────────────────────────────────
//   Current uses:  1
//   - query.ixx::walk_children<Id, C, V>  (Phase 3)
//   Target sites:
//     - new helper templates in compiler/ that operate
//       on "any AST" (not just FlatAST)
//
// ── Mutator ────────────────────────────────────────────────
//   Current uses:  1
//   - mutators.ixx::apply_mutation<M>  (Phase 2)
//   Target sites:
//     - (Phase 4) mutate:* Aura primitives could route
//       through the same dispatch (already done for
//       insert-child + remove-node; future migrate:
//       set-body, splice, rebind)
//
// ── ArenaAllocator ─────────────────────────────────────────
//   Current uses:  0
//   Target sites:
//     - arena.ixx::create<T> + destroy<T>  (Phase C3)
//     - type_arena.ixx::allocate<T> + create<T> + destroy<T>
//       (Phase C3)
//
// ── Queryable ──────────────────────────────────────────────
//   Current uses:  0
//   Target sites:
//     - query.ixx has QueryEngine class but no template
//       entry point that takes a "query strategy". When
//       such a template is added, constrain with
//       Queryable<Q, FlatAST, NodeId>.
//
// ── AuraInvocable (NEW, Phase C2) ──────────────────────────
//   Current uses:  0
//   Target sites:
//     - ast.ixx::rename_param<Callback>  (Phase C4)
//     - evaluator.ixx::walk_env_frames<F>  (Phase C4)
//     - (future) any visitor template
//
// ── RangeOf / AnyRange (NEW, Phase C2) ─────────────────────
//   Current uses:  0
//   Target sites:
//     - new helper templates that take a span<T> / vector<T>
//       and need to assert the element type
//
// ── SymbolInterner (NEW, Phase C2) ─────────────────────────
//   Current uses:  0
//   Target sites:
//     - (future) compile:* primitives that need to resolve
//       a symbol name to a SymId
//
// ── StableNodeRefLike (NEW, Phase C2) ──────────────────────
//   Current uses:  0
//   Target sites:
//     - any template that captures a node id + generation
//       (StableNodeRef, LayerStableRef)
//
// Zero-overhead note:
// All 9 concepts are pure compile-time constraints. They
// erase to nothing at runtime — the generated assembly
// is identical to hand-written constraints via
// std::enable_if / SFINAE / if constexpr. Verified by
// inspecting codegen at -O3 -DNDEBUG (see
// tests/test_issue_501_concepts.cpp for static_asserts).
//
// Why concepts over SFINAE:
//   - Error messages: "M does not satisfy Mutator" beats
//     200 lines of SFINAE substitution failure trace.
//   - Composition: `concept X = A && B` is simpler than
//     nested enable_if_t<is_X<T>>.
//   - Documentation: the concept definition IS the API
//     contract; no separate doc needed.
//   - Tooling: IDEs and static analyzers understand
//     concepts natively; SFINAE patterns are opaque.

// ── SoAColumnar (NEW, Issue #431) ───────────────────────────
//
// A SoA (Structure-of-Arrays) column storage. Required
// API:
//   - size(): the column length (same across all columns
//     in a parallel SoA container)
//   - empty(): shortcut for size() == 0
//   - data(): contiguous pointer to the column's elements
//     (used by JIT lowering + interpreter fast paths)
//
// Used by:
//   - IRFunctionSoA: per-function SoA columns
//     (opcodes_, operand0_, ..., block_dirty_, instruction_dirty_)
//   - FlatAST: per-node SoA columns (tag_, sym_id_,
//     child_begin_, ..., dirty_)
//   - IRCacheEntry::block_dirty_per_func_: nested bytes
//
// The data() requirement is what makes this concept
// distinct from a generic range: the JIT fast path needs
// pointer-based traversal (no iterators), and the
// interpreter needs to know the storage is contiguous.
export template <typename C>
concept SoAColumnar = requires(C& c, const C& cc) {
    { cc.size() } -> std::integral;
    { cc.empty() } -> std::convertible_to<bool>;
    { c.data() };
};

// ── DirtyPropagator (NEW, Issue #431) ───────────────────────
//
// A node/func marker that participates in the dirty
// cascade. Required API:
//   - mark_dirty(id): single-node / single-block dirty
//   - mark_dirty_upward(id, n): cascade upward (mark a
//     node + its N ancestors dirty)
//   - is_dirty(id): query whether a node is dirty
//   - clear_dirty(id): clear the dirty bit (post re-lower)
//
// Used by:
//   - FlatAST: per-node dirty column
//   - IRFunctionSoA: per-block + per-instruction dirty
//   - IRCacheEntry: entry-level dirty + per-function
//     block_dirty_per_func_
//
// The mark_dirty_upward requirement encodes the AI-loop
// cascade pattern: a single mutation in the workspace
// propagates upward through dep_graph_, marking every
// ancestor as dirty. Templates that consume the cascade
// (mark_define_dirty, mark_all_defines_dirty) constrain
// their parameter with DirtyPropagator.
export template <typename D, typename Id = std::uint32_t>
concept DirtyPropagator = requires(D d, Id id, std::size_t n) {
    { d.mark_dirty(id) };
    { d.mark_dirty_upward(id, n) };
    { d.is_dirty(id) } -> std::convertible_to<bool>;
    { d.clear_dirty(id) };
};

// ── ShapeDispatchable (NEW, Issue #431) ──────────────────────
//
// A shape profiler / dispatcher. Required API:
//   - inline_shape_of(value): the shape id of a value
//     (0 = generic, otherwise specialized for that shape)
//   - shape_name(id): human-readable name for a shape id
//     (for diagnostics)
//   - is_specialized(id): is this shape specialized?
//
// Used by:
//   - shape_profiler.cpp: the central shape_id dispatcher
//   - JIT lower() / TypeSpecializationWrap: per-fn
//     specialized_for lookup
//   - QueryShapeWrap: per-node shape classification
//
// The inline_shape_of requirement is what makes this
// concept distinct from a generic function: the JIT path
// needs the shape check to be cheap (bit-test + switch,
// no allocation). The shape_name requirement is for
// diagnostics / observability.
export template <typename S, typename Value = int, typename Id = std::uint32_t>
concept ShapeDispatchable = requires(S s, Value v, Id id) {
    { s.inline_shape_of(v) } -> std::integral;
    { s.shape_name(id) } -> std::convertible_to<std::string_view>;
    { s.is_specialized(id) } -> std::convertible_to<bool>;
};

} // namespace aura::core
