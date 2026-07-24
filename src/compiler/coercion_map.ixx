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
#include <atomic>
#include <cstdint>
#include <vector>
#include "core/provenance_tracker.hh" // Issue #2024: hygiene stamp + chain recovery

export module aura.compiler.coercion_map;

import aura.core.ast;

namespace aura::compiler {

// Issue #2024: forensic sentinel base for incomplete occurrence-narrowing
// provenance (high nibble C0E5 = "coercion"). Low 16 bits carry original_child
// so Agents can recover the site when both predicate and mutation were unset.
export inline constexpr std::uint32_t kCoercionProvenanceSentinelBase = 0xC0E50000u;

// Issue #2024: process-wide apply_coercion_map provenance completeness.
// Complete = both predicate_cond_node and source_mutation_id non-zero after
// full chain walk. Miss = needed sentinel / weak fallback. Ratio = complete /
// (complete + miss) as basis points (0–10000).
export inline std::atomic<std::uint64_t> g_coercion_provenance_complete_total{0};
export inline std::atomic<std::uint64_t> g_coercion_provenance_miss_total{0};
export inline std::atomic<std::uint64_t> g_coercion_provenance_sentinel_total{0};
export inline std::atomic<std::uint64_t> g_coercion_provenance_chain_walk_total{0};

export [[nodiscard]] inline std::uint64_t coercion_provenance_completeness_bp() noexcept {
    const auto c = g_coercion_provenance_complete_total.load(std::memory_order_relaxed);
    const auto m = g_coercion_provenance_miss_total.load(std::memory_order_relaxed);
    const auto d = c + m;
    return d > 0 ? (c * 10000u) / d : 10000u; // no samples → vacuously complete
}

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
    // Issue #537 / #518 Phase 2: optional occurrence-narrowing
    // provenance carried into apply_coercion_map. 0 = unset.
    std::uint32_t predicate_cond_node = 0;
    std::uint64_t source_mutation_id = 0;
    // Issue #691: narrowing-evidence bitmask for post-narrow
    // CastOp elision (stored on Coercion node float_val_).
    std::uint32_t narrow_evidence = 0;
};

// Issue #2024: walk provenance chain to fill missing CoercionEntry fields.
// Order: child column → parent walk → mutation log (target/parent match) →
// mutation log back() → hygiene tracker → sentinel. Never leaves both zero
// after a non-elided apply candidate is processed.
inline void fill_coercion_provenance_chain(aura::ast::FlatAST& flat, CoercionEntry& e) noexcept {
    using aura::ast::NULL_NODE;
    g_coercion_provenance_chain_walk_total.fetch_add(1, std::memory_order_relaxed);

    // 1. Child provenance column
    if (e.predicate_cond_node == 0 && e.original_child != NULL_NODE &&
        e.original_child < flat.size()) {
        const auto child_prov = flat.provenance(e.original_child);
        if (child_prov != 0)
            e.predicate_cond_node = child_prov;
    }

    // 2. Walk parent chain for first non-zero provenance (cross-delta
    // rewrite often leaves the child blank while an ancestor retains it).
    if (e.predicate_cond_node == 0 && e.original_child != NULL_NODE &&
        e.original_child < flat.size()) {
        auto cur = static_cast<aura::ast::NodeId>(e.original_child);
        for (int hops = 0; hops < 64; ++hops) {
            if (cur == NULL_NODE || cur >= flat.size())
                break;
            const auto p = flat.provenance(cur);
            if (p != 0) {
                e.predicate_cond_node = p;
                break;
            }
            const auto par = flat.parent_of(cur);
            if (par == cur || par == NULL_NODE)
                break;
            cur = par;
        }
    }

    // 3. Mutation log: prefer records targeting original_child / parent_id
    // (walk reverse = newest first). Also follow parent_mutation_id once for
    // composite / multi-delta root attribution.
    const auto& log = flat.all_mutations();
    if (e.source_mutation_id == 0 && !log.empty()) {
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            if (it->target_node == e.original_child || it->target_node == e.parent_id ||
                it->parent_id == e.parent_id ||
                (e.original_child != 0 && it->parent_id == e.original_child)) {
                e.source_mutation_id = it->mutation_id;
                if (e.predicate_cond_node == 0 && it->target_node != 0 &&
                    it->target_node != NULL_NODE) {
                    e.predicate_cond_node = static_cast<std::uint32_t>(it->target_node);
                }
                // Multi-delta: if this record has a parent mutation, prefer
                // the root when the entry still has no predicate.
                if (it->parent_mutation_id != 0 && e.predicate_cond_node == 0) {
                    for (const auto& r : log) {
                        if (r.mutation_id == it->parent_mutation_id && r.target_node != 0) {
                            e.predicate_cond_node = static_cast<std::uint32_t>(r.target_node);
                            break;
                        }
                    }
                }
                break;
            }
        }
        if (e.source_mutation_id == 0)
            e.source_mutation_id = log.back().mutation_id;
    }

    // 4. Hygiene tracker (MacroIntroduced / audit path)
    if (e.predicate_cond_node == 0 || e.source_mutation_id == 0) {
        const auto& hy = aura::core::provenance::g_provenance_tracker().last_hygiene;
        if (e.predicate_cond_node == 0 && hy.node_id != 0)
            e.predicate_cond_node = hy.node_id;
        if (e.source_mutation_id == 0 && hy.source_mutation_id != 0)
            e.source_mutation_id = hy.source_mutation_id;
    }

    // 5. Completeness vs sentinel forensic recovery
    const bool complete = e.predicate_cond_node != 0 && e.source_mutation_id != 0;
    if (complete) {
        g_coercion_provenance_complete_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_coercion_provenance_miss_total.fetch_add(1, std::memory_order_relaxed);
    if (e.predicate_cond_node == 0) {
        // Sentinel: always non-zero; low bits = original_child for recovery.
        const auto low = static_cast<std::uint32_t>(e.original_child & 0xFFFFu);
        e.predicate_cond_node = kCoercionProvenanceSentinelBase | (low == 0 ? 1u : low);
        g_coercion_provenance_sentinel_total.fetch_add(1, std::memory_order_relaxed);
    }
    if (e.source_mutation_id == 0) {
        // Weak mutation stamp: original_child (or 1) so deopt/rollback trails
        // never see a zero mutation id after apply.
        e.source_mutation_id =
            e.original_child != 0 ? static_cast<std::uint64_t>(e.original_child) : 1ull;
    }
}

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
                                         type_id, src_line, src_col, 0, 0});
    }

    // Issue #537: overload with occurrence-narrowing provenance.
    void add(aura::ast::NodeId parent, std::uint32_t child_index, aura::ast::NodeId original_child,
             std::uint32_t type_tag, std::uint32_t type_id, std::uint32_t src_line,
             std::uint32_t src_col, std::uint32_t predicate_cond_node,
             std::uint64_t source_mutation_id, std::uint32_t narrow_evidence = 0) {
        entries_.push_back(CoercionEntry{static_cast<std::uint32_t>(parent), child_index,
                                         static_cast<std::uint32_t>(original_child), type_tag,
                                         type_id, src_line, src_col, predicate_cond_node,
                                         source_mutation_id, narrow_evidence});
    }

    const std::vector<CoercionEntry>& entries() const { return entries_; }
    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    void clear() {
        entries_.clear();
        eliminated_count_ = 0;
    }

    // Merge another map's entries into this one. Order is
    // preserved (other entries appended after this map's).
    void merge(const CoercionMap& other) {
        entries_.insert(entries_.end(), other.entries_.begin(), other.entries_.end());
    }

    // Issue #1425: count of identity coercions elided by
    // apply_coercion_map (not inserted as CoercionNodes).
    // Reset on clear(); bumped by mark_eliminated().
    [[nodiscard]] std::size_t eliminated_count() const noexcept { return eliminated_count_; }
    void mark_eliminated(std::size_t n = 1) noexcept { eliminated_count_ += n; }

private:
    std::vector<CoercionEntry> entries_;
    std::size_t eliminated_count_ = 0;
};

// Issue #1425: stats from apply_coercion_map with identity elision.
// `eliminated` maps to dead_coercion_eliminated metrics when the
// caller integrates (AST-level pre-IR win, complementary to
// DeadCoercionEliminationPass on IR CastOps).
export struct DeadCoercionAstStats {
    std::size_t applied = 0;       // CoercionNodes actually inserted
    std::size_t eliminated = 0;    // identity coercions skipped
    std::size_t kept = 0;          // alias of applied (non-identity)
    std::size_t skipped_stale = 0; // parent slot already rewritten / missing
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
// Issue #1425: identity elision — when the original child
// already carries `type_id == entry.type_id` (non-zero), the
// CoercionNode would lower to a no-op CastOp. Skip insertion
// entirely (defense-in-depth with IR DeadCoercionEliminationPass).
//
// Returns the number of entries actually applied (rest are
// skipped or elided). When `stats_out` is non-null, fills
// applied / eliminated / skipped_stale. When `map_mut` is
// non-null, bumps map_mut->mark_eliminated for identity skips.
export std::size_t apply_coercion_map(aura::ast::FlatAST& flat, const CoercionMap& map,
                                      DeadCoercionAstStats* stats_out = nullptr,
                                      CoercionMap* map_mut = nullptr) {
    DeadCoercionAstStats local_stats;
    auto& s = stats_out ? *stats_out : local_stats;
    s = {};

    for (const auto& e_in : map.entries()) {
        CoercionEntry e = e_in;

        // Issue #1425 / #1925: identity coercion — child already has the
        // target type stamped (post-infer). Do not insert a
        // CoercionNode; the IR path would only produce a dead CastOp.
        // Also elide Dynamic-target tags (type_tag==3): CastOp default
        // is passthrough; narrow_evidence-only identity when types match.
        if (e.type_id != 0 && flat.type_id(e.original_child) == e.type_id) {
            ++s.eliminated;
            if (map_mut)
                map_mut->mark_eliminated();
            continue;
        }

        // Issue #1925: Dynamic passthrough tag (3) with no meaningful
        // runtime check — skip CoercionNode insertion.
        if (e.type_tag == 3) {
            ++s.eliminated;
            if (map_mut)
                map_mut->mark_eliminated();
            continue;
        }

        // Issue #1873 / #2024: full provenance chain recovery for entries
        // that will be applied. Walk child → parent → mutation log →
        // hygiene; stamp sentinel when still incomplete.
        fill_coercion_provenance_chain(flat, e);

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
            if (e.narrow_evidence != 0)
                flat.set_float(coercion_id, static_cast<double>(e.narrow_evidence));
            // Issue #1873 / #2024: always stamp non-zero provenance after
            // fill_coercion_provenance_chain (predicate preferred; mutation
            // / sentinel as fallback).
            if (e.predicate_cond_node != 0)
                flat.set_provenance(coercion_id, e.predicate_cond_node);
            else if (e.source_mutation_id != 0)
                flat.set_provenance(coercion_id,
                                    static_cast<std::uint32_t>(e.source_mutation_id & 0xFFFFFFFFu));
            ++s.applied;
            ++s.kept;
            continue;
        }

        auto parent_v = flat.get(e.parent_id);
        if (e.child_index >= parent_v.children.size()) {
            // Stale entry — slot no longer exists. Skip.
            ++s.skipped_stale;
            continue;
        }
        if (parent_v.child(e.child_index) != e.original_child) {
            // Already applied, or another pass rewrote. Skip.
            ++s.skipped_stale;
            continue;
        }

        // Build the CoercionNode wrapping the original child.
        auto coercion_id = flat.add_coercion(e.original_child, e.type_tag, e.type_id);
        flat.set_loc(coercion_id, e.src_line, e.src_col);
        // Issue #691 / #1873 / #2024: stamp narrowing evidence + recovered
        // predicate/mutation provenance (never leave zero after chain walk).
        if (e.narrow_evidence != 0)
            flat.set_float(coercion_id, static_cast<double>(e.narrow_evidence));
        if (e.predicate_cond_node != 0)
            flat.set_provenance(coercion_id, e.predicate_cond_node);
        else if (e.source_mutation_id != 0)
            flat.set_provenance(coercion_id,
                                static_cast<std::uint32_t>(e.source_mutation_id & 0xFFFFFFFFu));
        // Rewrite the parent's child_index to point at the
        // new CoercionNode.
        flat.set_child(e.parent_id, e.child_index, coercion_id);
        ++s.applied;
        ++s.kept;
    }
    return s.applied;
}

} // namespace aura::compiler
