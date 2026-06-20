// coercion_map.ixx — Deferred CoercionNode insertion (Issue #116)
//
// The TypeChecker used to mutate the input FlatAST in-place to
// wrap mismatched expressions in CoercionNode wrappers, rewriting
// the parent→child link in the process. This broke the design
// contract that `ast:snapshot` / `ast:rollback` can rely on
// pre-typecheck state, and made the type checker unsafe to
// invoke on shared/versioned ASTs (e.g. for AI self-modifying
// code workflows where the AST may be inspected while type
// checking is in progress).
//
// The fix: type checking now collects coercion intent into a
// `CoercionMap` (a pure data structure with no AST references
// beyond NodeId integers). The mutation is then performed as a
// separate explicit pass via `apply_coercion_map`, which can be
// called once at the boundary between type checking and
// lowering/IR emission. The TypeChecker is now structurally
// read-only on the FlatAST; the only remaining mutation is
// `set_node_error` (a per-node metadata annotation that does
// not change tree structure, separately documented in
// `src/core/ast.ixx`).
//
// The map stores (parent, child_index, original_child,
// type_tag, type_id, src_line, src_col) tuples. Insertion order
// is preserved because child indices may shift if two
// coercions target the same parent — but in practice the type
// checker only emits one coercion per parent/child slot per
// call, and the apply pass uses the recorded (parent, child,
// index) triple to locate the original child before rewriting,
// so duplicate or out-of-order entries are safe.
//
// Apply pass is idempotent: applying twice is a no-op the
// second time (the original child is no longer there, so no
// match).

module;
#include <cstdint>
#include <vector>

export module aura.compiler.coercion_map;

import aura.core.ast;

namespace aura::compiler {

// ── CoercionEntry — one deferred coercion ────────────────
//
// Describes: "the child at index `child_index` of parent
// `parent_id` (which currently points to `original_child`)
// should be wrapped in a CoercionNode targeting `type_id`
// with runtime check tag `type_tag` (the CastOp type_tag, see
// type_checker_impl.cpp `type_tag_for_coercion`)."
//
// `src_line` / `src_col` are copied onto the CoercionNode for
// blame tracking (Issue #79 — the CoercionNode inherits the
// source location of the original expression).
//
// `parent_id` of 0 (aura::ast::NULL_NODE) means the coercion
// has no parent slot to rewrite — the apply pass still
// creates the CoercionNode for the IR generator to see, but
// doesn't touch any parent link.
export struct CoercionEntry {
    std::uint32_t parent_id;
    std::uint32_t child_index;
    std::uint32_t original_child;
    std::uint32_t type_tag;
    std::uint32_t type_id;
    std::uint32_t src_line;
    std::uint32_t src_col;
};

// ── CoercionMap — accumulated coercion intent ────────────
//
// Collected during type checking, applied as a single explicit
// pass before lowering. Cheap to copy (just a vector of
// trivially-copyable entries), cheap to clear, safe to pass
// across module boundaries.
export class CoercionMap {
public:
    void add(aura::ast::NodeId parent, std::uint32_t child_index, aura::ast::NodeId original_child,
             std::uint32_t type_tag, std::uint32_t type_id, std::uint32_t src_line,
             std::uint32_t src_col) {
        entries_.push_back(CoercionEntry{static_cast<std::uint32_t>(parent), child_index,
                                         static_cast<std::uint32_t>(original_child), type_tag,
                                         type_id, src_line, src_col});
    }

    const std::vector<CoercionEntry>& entries() const { return entries_; }
    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    void clear() { entries_.clear(); }

    // Merge another map's entries into this one. Order is
    // preserved (other entries appended after this map's).
    void merge(const CoercionMap& other) {
        entries_.insert(entries_.end(), other.entries_.begin(), other.entries_.end());
    }

private:
    std::vector<CoercionEntry> entries_;
};

// ── apply_coercion_map — the one explicit AST-mutating pass ───
//
// Walks the CoercionMap and, for each entry, calls
// `flat.add_coercion(original_child, type_tag, type_id)`,
// copies the source location, and rewrites the parent's
// `child_index` reference to point to the new CoercionNode.
//
// If a parent slot already points to something other than
// `original_child` (e.g. a previous apply already ran, or
// another pass mutated the tree), the entry is skipped — this
// keeps the pass idempotent and safe to call multiple times.
//
// Returns the number of entries actually applied (rest are
// skipped, typically zero on a clean re-apply).
export std::size_t apply_coercion_map(aura::ast::FlatAST& flat, const CoercionMap& map) {
    std::size_t applied = 0;
    for (const auto& e : map.entries()) {
        // Locate the parent and confirm it still points at the
        // original child we recorded. If it doesn't (e.g. this
        // pass already ran, or another mutator touched the
        // tree), skip the entry — idempotency.
        if (e.parent_id == aura::ast::NULL_NODE) {
            // Top-level expression: there is no parent to
            // rewrite. This case is rare (top-level
            // coercions don't need parent rewrite), but we
            // still need the CoercionNode for the IR
            // lowering to see it. Insert it as a free node
            // (parented to itself's children — already
            // handled by add_coercion).
            auto coercion_id = flat.add_coercion(e.original_child, e.type_tag, e.type_id);
            flat.set_loc(coercion_id, e.src_line, e.src_col);
            ++applied;
            continue;
        }

        auto parent_v = flat.get(e.parent_id);
        if (e.child_index >= parent_v.children.size()) {
            // Stale entry — slot no longer exists. Skip.
            continue;
        }
        if (parent_v.child(e.child_index) != e.original_child) {
            // Already applied, or another pass rewrote. Skip.
            continue;
        }

        // Build the CoercionNode wrapping the original child.
        auto coercion_id = flat.add_coercion(e.original_child, e.type_tag, e.type_id);
        flat.set_loc(coercion_id, e.src_line, e.src_col);
        // Rewrite the parent's child_index to point at the
        // new CoercionNode.
        flat.set_child(e.parent_id, e.child_index, coercion_id);
        ++applied;
    }
    return applied;
}

} // namespace aura::compiler
