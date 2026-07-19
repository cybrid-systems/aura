// evaluator_primitives_compile_05.cpp — Issue #909: peeled compile registration
// aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "per_defuse_index.h"
#include "hash_meta.h"
#include "basis_points.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.mutators;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.value;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.pass_manager;
import aura.compiler.query;
import aura.compiler.hardware_backend;
import aura.compiler.sv_ir;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_keyword;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

// Issue #1787: shared FNV-1a + open-addressing stats hash builder.
// Replaces 6× inlined build_hash lambdas in this file. Capacity
// scales with kv count (load factor ≤ 0.5, minimum 16 slots).
[[nodiscard]] EvalValue build_kv_hash(Evaluator& ev,
                                      std::span<const std::pair<std::string, EvalValue>> kv) {
    std::size_t ncap = 16;
    while (ncap < kv.size() * 2)
        ncap *= 2;
    auto* ht = FlatHashTable::create(ncap);
    if (!ht)
        return make_void();
    auto meta = ht->metadata();
    auto keys = ht->keys();
    auto vals = ht->values();
    auto hcap = ht->capacity;
    for (auto& [k, v] : kv) {
        std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
        for (char c : k)
            h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
        auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
        if (fp == 0xFF)
            fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
        const auto kidx = static_cast<std::uint64_t>(ev.push_string_heap(k));
        bool inserted = false;
        for (std::size_t at = 0; at < hcap; ++at) {
            auto idx = ((h >> 1) + at) & (hcap - 1);
            if (meta[idx] == 0xFF) {
                meta[idx] = fp;
                keys[idx] = make_string(kidx).val;
                vals[idx] = v.val;
                ht->size++;
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            FlatHashTable::destroy(ht);
            return make_void();
        }
    }
    auto hidx = g_hash_tables.size();
    g_hash_tables.push_back(ht);
    return make_hash(hidx);
}

// Issue #909 compile part 40 (orig 3280-3340)
void CompilePrims::register_compile_p40(PrimRegistrar add, Evaluator& ev) {

    ObservabilityPrims::register_stats_impl(
        "compile:inline-pass-stats", [&ev](const auto&) -> EvalValue {
            std::int64_t inlined = 0;
            std::int64_t branch_aware = 0;
            if (ev.get_inline_stats_fn_) {
                // Issue #1784: unpack via uint32_t so each half is
                // always non-negative when widened to int64_t.
                // Direct static_cast<int64_t>(packed & 0xFFFFFFFF)
                // is well-defined for uint64_t, but going through
                // uint32_t makes the "no sign bit" contract explicit
                // for agents / future refactors that might cast
                // through int32_t by mistake.
                const std::uint64_t packed = ev.get_inline_stats_fn_();
                const std::uint32_t inlined_u32 = static_cast<std::uint32_t>(packed & 0xFFFFFFFFu);
                const std::uint32_t branch_aware_u32 = static_cast<std::uint32_t>(packed >> 32);
                inlined = static_cast<std::int64_t>(inlined_u32);
                branch_aware = static_cast<std::int64_t>(branch_aware_u32);
            }
            std::int64_t macro_skipped = 0;
            if (ev.get_macro_hygiene_skipped_fn_) {
                macro_skipped = static_cast<std::int64_t>(ev.get_macro_hygiene_skipped_fn_());
            }
            std::int64_t total = inlined + branch_aware;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"inlined", make_int(inlined)},
                {"branch-aware", make_int(branch_aware)},
                {"macro-hygiene-skipped", make_int(macro_skipped)},
                {"total", make_int(total)},
            };
            return build_kv_hash(ev, kv);
        });
}

// Issue #909 compile part 41 (orig 3341-3412)
void CompilePrims::register_compile_p41(PrimRegistrar add, Evaluator& ev) {

    // (concurrency:stats) — Issue #189 (P0): concurrency safety
    // observability. Reports the current defuse_version_ (the
    // monotonic mutation counter bumped on every mutate:*), the
    // total number of mutations ever applied to this evaluator
    // (the issue's "mutation count" stat), the per-join wait
    // snapshot, and the MutationBoundaryGuard stack depth.
    //
    // The hash has 4 keys:
    //   defuse-version:    uint64 (acquire-loaded for safety)
    //   total-mutations:   uint64 (lifetime count)
    //   boundary-depth:    int (current MutationBoundaryGuard stack size)
    //   at-wait-version:   uint64 (per-join snapshot, 0 if no active wait)
    //
    // Use (concurrency:stats) to:
    //   - verify a (mutate:*) actually bumped the version
    //   - count how many mutations a workload has applied
    //   - debug concurrent fiber contention via boundary-depth
    ObservabilityPrims::register_stats_impl("concurrency:stats", [&ev](const auto&) -> EvalValue {
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"defuse-version", make_int(static_cast<std::int64_t>(ev.defuse_version_snapshot()))},
            {"total-mutations", make_int(static_cast<std::int64_t>(ev.total_mutations()))},
            {"boundary-depth", make_int(static_cast<std::int64_t>(ev.mutation_boundary_depth()))},
            {"at-wait-version", make_int(static_cast<std::int64_t>(ev.defuse_version_at_wait_))},
            {"mutation-yield-count",
             make_int(static_cast<std::int64_t>(ev.mutation_yield_count()))},
            {"compaction-paused-by-boundary",
             make_int(static_cast<std::int64_t>(ev.compaction_paused_by_boundary()))},
            {"cross-fiber-rollback-count",
             make_int(static_cast<std::int64_t>(ev.cross_fiber_rollback_count()))},
        };
        return build_kv_hash(ev, kv);
    });
}

// Issue #909 compile part 42 (orig 3413-3482)
void CompilePrims::register_compile_p42(PrimRegistrar add, Evaluator& ev) {

    // (concurrency:version-snapshot) — Issue #189: capture the
    // current defuse_version_ and return it as an int. Use with
    // (concurrency:version-current? snap) to detect concurrent
    // mutations between two points in the program.
    ObservabilityPrims::register_stats_impl(
        "concurrency:version-snapshot", [&ev](const auto&) -> EvalValue {
            return make_int(static_cast<std::int64_t>(ev.defuse_version_snapshot()));
        });

    // (concurrency:version-current? snap) — Issue #189: returns
    // #t if the defuse_version_ has not changed since `snap` was
    // captured. #f if a mutation has happened (and AST/cells/pairs
    // may be stale).
    ObservabilityPrims::register_stats_impl(
        "concurrency:version-current?", [&ev](const auto& a) -> EvalValue {
            if (a.empty() || !is_int(a[0]))
                return make_bool(false);
            auto snap = static_cast<std::uint64_t>(as_int(a[0]));
            return make_bool(ev.is_version_current(snap));
        });

    // (syntax-marker node-id) — Issue #190: return the SyntaxMarker
    // value of a node (0=User, 1=MacroIntroduced, 2=BoolLiteral).
    // Used for EDSL filter queries (e.g., "find all macro-introduced
    // nodes") and for diagnostic output ("why did mutate:rebind
    // refuse to edit this node?").
    add("syntax-marker", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        if (!ev.workspace_flat_)
            return make_int(0);
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (id >= ev.workspace_flat_->size())
            return make_int(0);
        // Issue #1783: shared metadata lock vs concurrent set-marker.
        auto rlock = ev.workspace_flat_->try_acquire_metadata_reader_lock();
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->marker(id)));
    });

    // (syntax:set-marker node-id marker) — Issue #366: set the
    // SyntaxMarker value of a node. Returns true on success.
    // marker is an integer (0=User, 1=MacroIntroduced,
    // 2=BoolLiteral). Used by EDSL transformers and auditability
    // tooling to correct marker drift after a manual mutation
    // sequence. The primitive does NOT propagate the marker to
    // children — use syntax:propagate-marker for that.
    add("syntax:set-marker", [&ev](const auto& a) -> EvalValue {
        // Issue #1002: removed dead `bool ok` (error paths return merr
        // immediately; ok was never read).
        if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1])) {
            return ev.make_merr("bad-arg", "usage: (syntax:set-marker node-id marker)");
        }
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto marker_val = static_cast<int>(as_int(a[1]));
        if (!ev.workspace_flat_) {
            return ev.make_merr("no-workspace", "no active workspace");
        }
        if (id >= ev.workspace_flat_->size()) {
            return ev.make_merr("out-of-range", "node id " + std::to_string(id) + " >= flat size");
        }
        if (marker_val < 0 || marker_val > 2) {
            return ev.make_merr("bad-arg",
                                "marker must be 0 (User), 1 (MacroIntroduced), or 2 (BoolLiteral)");
        }
        // No MutationBoundaryGuard — metadata-only (no generation
        // bump). Issue #1783: exclusive metadata_mtx_ serializes
        // cross-fiber marker_column writes without invalidating
        // StableNodeRef / marker-query caches.
        auto wlock = ev.workspace_flat_->begin_metadata_mutation();
        ev.workspace_flat_->set_marker(id, static_cast<aura::ast::SyntaxMarker>(marker_val));
        return make_bool(true);
    });
}

// Issue #909 compile part 43 (orig 3483-3546)
void CompilePrims::register_compile_p43(PrimRegistrar add, Evaluator& ev) {

    // (syntax:propagate-marker node-id marker) — Issue #366:
    // set the marker on a node AND recursively on all
    // descendants. Returns the count of nodes updated. Used by
    // EDSL transformers to re-stamp a macro-introduced subtree
    // after a structural mutation (insert-child, replace-subtree)
    // that may have lost the original marker on some children.
    // The primitive does NOT bump defuse_version_ — marker
    // changes are observational metadata, not workspace state.
    //
    // Issue #1782: FlatAST children can form cycles (DAG / self-
    // mutate). Dense seen[] + kMaxVisit abort (parity #1679 /
    // #1682) prevents unbounded stack growth on cycles.
    add("syntax:propagate-marker", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1])) {
            return make_int(0);
        }
        auto root = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto marker_val = static_cast<int>(as_int(a[1]));
        if (!ev.workspace_flat_)
            return make_int(0);
        if (root == aura::ast::NULL_NODE || root >= ev.workspace_flat_->size())
            return make_int(0);
        if (marker_val < 0 || marker_val > 2)
            return make_int(0);
        auto& flat = *ev.workspace_flat_;
        // Iterative DFS with visited set — metadata-only.
        // Issue #1783: hold exclusive metadata_mtx_ for the whole
        // walk so concurrent set-marker / get-marker cannot tear.
        auto wlock = flat.begin_metadata_mutation();
        std::int64_t count = 0;
        std::vector<aura::ast::NodeId> stack;
        std::vector<std::uint8_t> seen(flat.size(), 0);
        stack.push_back(root);
        seen[static_cast<std::size_t>(root)] = 1;
        std::size_t visited = 1;
        const std::size_t kMaxVisit = flat.size();
        while (!stack.empty()) {
            auto cur = stack.back();
            stack.pop_back();
            if (cur == aura::ast::NULL_NODE || cur >= flat.size())
                continue;
            flat.set_marker(cur, static_cast<aura::ast::SyntaxMarker>(marker_val));
            ++count;
            const auto& children = flat.children(cur);
            for (std::uint32_t ci = 0; ci < children.size(); ++ci) {
                const auto c = children[ci];
                if (c == aura::ast::NULL_NODE || c >= flat.size())
                    continue;
                const auto cix = static_cast<std::size_t>(c);
                if (seen[cix])
                    continue; // cycle edge or shared DAG child
                seen[cix] = 1;
                ++visited;
                if (visited > kMaxVisit)
                    return make_int(count); // defensive abort
                stack.push_back(c);
            }
        }
        return make_int(count);
    });

    // (syntax:set-provenance node-id prov-id) — Issue #367:
    // set the per-node provenance id. The prov-id is a
    // workspace-scoped identifier that AI agents can use to
    // trace "this node came from macro X during expansion Y".
    // The actual provenance data (macro_def_id, expansion_id,
    // mutation_id) lives in a side-table that the host can
    // populate out-of-band; this primitive only stores the
    // index. 0 = no provenance recorded.
    add("syntax:set-provenance", [&ev](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]))
            return make_bool(false);
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto prov = static_cast<std::uint32_t>(as_int(a[1]));
        if (!ev.workspace_flat_)
            return make_bool(false);
        if (id >= ev.workspace_flat_->size())
            return make_bool(false);
        // Issue #1783: exclusive metadata_mtx_ for provenance column.
        auto wlock = ev.workspace_flat_->begin_metadata_mutation();
        ev.workspace_flat_->set_provenance(id, prov);
        return make_bool(true);
    });
}

// Issue #909 compile part 44 (orig 3547-3630)
void CompilePrims::register_compile_p44(PrimRegistrar add, Evaluator& ev) {

    // (syntax:get-provenance node-id) — Issue #367: return
    // the per-node provenance id (0 if unset). The host can
    // look up the actual macro_def_id / expansion_id /
    // mutation_id via its own side-table.
    add("syntax:get-provenance", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        if (!ev.workspace_flat_)
            return make_int(0);
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (id >= ev.workspace_flat_->size())
            return make_int(0);
        // Issue #1783: shared metadata lock vs concurrent set-provenance.
        auto rlock = ev.workspace_flat_->try_acquire_metadata_reader_lock();
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->provenance(id)));
    });

    // (syntax-marker-counts) — Issue #190: aggregate count of
    // each SyntaxMarker value across the workspace. Hash with
    // 3 integer fields: user, macro-introduced, bool-literal,
    // plus total-nodes. Useful for dashboards ("how much of the
    // workspace is macro-introduced code?") and for asserting
    // hygiene invariants in tests.
    ObservabilityPrims::register_stats_impl(
        "syntax-marker-counts", [&ev](const auto&) -> EvalValue {
            if (!ev.workspace_flat_)
                return make_void();
            std::size_t user = 0, macro = 0, bool_lit = 0, total = 0;
            // Issue #1783: shared lock for full-column scan.
            auto rlock = ev.workspace_flat_->try_acquire_metadata_reader_lock();
            const auto& markers = ev.workspace_flat_->marker_column();
            for (std::size_t i = 0; i < markers.size(); ++i) {
                ++total;
                auto m = static_cast<int>(markers[i]);
                if (m == 0)
                    ++user;
                else if (m == 1)
                    ++macro;
                else if (m == 2)
                    ++bool_lit;
            }
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"user", make_int(static_cast<std::int64_t>(user))},
                {"macro-introduced", make_int(static_cast<std::int64_t>(macro))},
                {"bool-literal", make_int(static_cast<std::int64_t>(bool_lit))},
                {"total-nodes", make_int(static_cast<std::int64_t>(total))},
            };
            return build_kv_hash(ev, kv);
        });
}

// Issue #909 compile part 45 (orig 3631-3794)
void CompilePrims::register_compile_p45(PrimRegistrar add, Evaluator& ev) {

    // (compile:per-symbol-dirty-stats sym) — Issue #410: per-symbol
    // dirty observability. Returns a hash with 4 fields:
    //   - per-symbol-affected-count: number of Variable nodes in
    //     the flat whose sym_id matches `sym` (the per-symbol
    //     affected set)
    //   - ancestor-affected-count: number of nodes in the
    //     ancestor chain of the def node (the legacy
    //     mark_dirty_upward set; -1 if the def node is not
    //     found in the flat — conservative unknown)
    //   - reduction-ratio-bp: per-symbol / ancestor * ::aura::compiler::kBasisPointScale in
    //     basis points. Higher = bigger savings if #410 Phase 2
    //     wires affected_subtree_for_symbol into infer_flat_partial.
    //     10000 = per-symbol set is the same size as ancestor set
    //     (no savings). 0 = per-symbol set is empty (no uses).
    //   - lookup-count: cumulative per-symbol-dirty lookups
    //     (lifetime total from metrics_).
    //
    // ACs:
    //   AC1: counter starts at 0
    //   AC2: primitive returns hash with 4 keys
    //   AC3: per-symbol < ancestor-affected on a body with 5+ bindings
    //   AC4: counter increments after a primitive call
    //   AC5: unbound sym returns sensible (0,0,0,0) values
    //   AC6: reduction-ratio-bp matches manual calculation
    // Multi-arg (sym name required) — must stay public add();
    // stats:get/engine:metrics cannot pass the symbol argument.
    add("compile:per-symbol-dirty-stats", [&ev](const auto& a) -> EvalValue {
        // Issue #1787: build_kv_hash shared helper.
        // Resolve sym name → SymId. Use the workspace pool +
        // string heap (same pattern as query:def-use).
        if (a.empty() || !is_string(a[0]))
            return ev.make_merr("bad-arg", "usage: (compile:per-symbol-dirty-stats sym-name)");
        auto sym_idx = as_string_idx(a[0]);
        std::string sym_name;
        if (sym_idx < ev.string_heap_.size()) {
            sym_name = ev.string_heap_[sym_idx];
        } else {
            return ev.make_merr("bad-arg", "symbol name string index out of range");
        }
        // Issue #1785: hold workspace_mtx_ for pool lookup + FlatAST
        // walks so concurrent mutate / intern cannot race. Prefer
        // find_by_name (read-only) over intern(write) — unbound
        // names stay INVALID_SYM and return zeroed stats (AC5)
        // without mutating the pool hash table.
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        aura::ast::SymId target_sym = aura::ast::INVALID_SYM;
        if (ev.workspace_flat_ && ev.workspace_pool_) {
            if (auto found = ev.workspace_pool_->find_by_name(sym_name))
                target_sym = *found;
        }
        // Compute per-symbol affected set (O(n) walk).
        std::vector<aura::ast::NodeId> per_symbol_affected;
        if (ev.workspace_flat_ && target_sym != aura::ast::INVALID_SYM) {
            per_symbol_affected = affected_subtree_for_symbol(*ev.workspace_flat_, target_sym);
        }
        // Compute ancestor-affected count: walk the parent_ chain
        // from the def node (the Define/Let/LetRec that binds
        // `target_sym`). If no def node is found, report -1 (unknown).
        std::int64_t ancestor_affected = -1;
        if (ev.workspace_flat_ && target_sym != aura::ast::INVALID_SYM) {
            aura::ast::NodeId def_node = aura::ast::NULL_NODE;
            const std::size_t n = ev.workspace_flat_->size();
            for (std::size_t i = 0; i < n; ++i) {
                auto v = ev.workspace_flat_->get(static_cast<aura::ast::NodeId>(i));
                if ((v.tag == aura::ast::NodeTag::Define || v.tag == aura::ast::NodeTag::Let ||
                     v.tag == aura::ast::NodeTag::LetRec) &&
                    v.sym_id == target_sym) {
                    def_node = static_cast<aura::ast::NodeId>(i);
                    break;
                }
            }
            if (def_node != aura::ast::NULL_NODE) {
                // Walk up the parent chain. mark_dirty_upward would
                // also include descendants of each ancestor; we
                // report the chain length only (the conservative
                // ancestor-only count, which is what the per-symbol
                // set needs to beat to justify the new path).
                //
                // Phase A1 migration: now uses
                // aura::compiler::walk_ancestors<Id, C, V> from
                // aura.compiler.query. The walk starts from
                // parent_of(def_node) to match the original semantics
                // (count ancestors of def_node, excluding def_node
                // itself). The size()-bounded safety cap is preserved
                // inside the visitor via early-return.
                //
                // Issue #1786: dense seen[] stops parent_of cycles so
                // each ancestor is counted at most once (max_count alone
                // still overcounts cycles shorter than flat.size()).
                std::int64_t chain_len = 0;
                auto start = ev.workspace_flat_->parent_of(def_node);
                const auto max_count = static_cast<std::size_t>(ev.workspace_flat_->size());
                if (start != aura::ast::NULL_NODE) {
                    std::vector<std::uint8_t> seen(ev.workspace_flat_->size(), 0);
                    chain_len =
                        static_cast<std::int64_t>(aura::compiler::walk_ancestors<std::uint32_t>(
                            *ev.workspace_flat_, start,
                            [&seen, &chain_len, max_count](aura::ast::NodeId cur) -> bool {
                                if (static_cast<std::size_t>(chain_len) >= max_count)
                                    return false; // safety cap
                                if (cur >= seen.size())
                                    return false;
                                const auto ci = static_cast<std::size_t>(cur);
                                if (seen[ci])
                                    return false; // cycle — stop, do not re-count
                                seen[ci] = 1;
                                ++chain_len;
                                return true;
                            }));
                }
                ancestor_affected = chain_len;
            }
        }
        rlock.unlock(); // done with pool + flat; metrics are atomics
        // Bump metrics_.
        std::uint64_t lookup_count = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            m->per_symbol_dirty_lookups_total.fetch_add(1, std::memory_order_relaxed);
            m->per_symbol_dirty_uses_total.fetch_add(
                static_cast<std::uint64_t>(per_symbol_affected.size()), std::memory_order_relaxed);
            lookup_count = m->per_symbol_dirty_lookups_total.load(std::memory_order_relaxed);
        }
        // reduction-ratio-bp = per_symbol / ancestor * ::aura::compiler::kBasisPointScale.
        // Cap at 10000 (per_symbol can't exceed ancestor in
        // practice, but defensive). Use 0 when ancestor is 0/-
        std::int64_t ratio_bp = 0;
        if (ancestor_affected > 0 && !per_symbol_affected.empty()) {
            const auto num = static_cast<std::int64_t>(per_symbol_affected.size());
            ratio_bp = (num * ::aura::compiler::kBasisPointScale) / ancestor_affected;
            if (ratio_bp > 10000)
                ratio_bp = 10000;
        }
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"per-symbol-affected-count",
             make_int(static_cast<std::int64_t>(per_symbol_affected.size()))},
            {"ancestor-affected-count", make_int(ancestor_affected)},
            {"reduction-ratio-bp", make_int(ratio_bp)},
            {"lookup-count", make_int(static_cast<std::int64_t>(lookup_count))},
        };
        return build_kv_hash(ev, kv);
    });
}

// Issue #909 compile part 46 (orig 3795-3871)
void CompilePrims::register_compile_p46(PrimRegistrar add, Evaluator& ev) {

    // (compile:incremental-typecheck-stats) — Issue #411: post-
    // mutation auto-incremental typecheck observability. Returns
    // a hash with 3 fields:
    //   - auto-invocations-total: lifetime total number of
    //     typed_mutate success paths that triggered an automatic
    //     infer_flat_partial call. 0 in Lazy/Disabled modes.
    //   - re-inferred-total: cumulative count of nodes re-
    //     inferred across all auto-invocations.
    //   - avg-re-inferred-bp: derived average (re_inferred *
    //     10000 / max(auto_invocations, 1)) in basis points.
    //     Higher = more nodes re-inferred per mutation on
    //     average. The follow-up per-symbol wiring (Issue #410
    //     Phase 2/2) will reduce this metric.
    //
    // Mirrors the 2 lifetime counters on CompilerMetrics plus
    // the derived metric on CompilerSnapshot (Issue #1787:
    // build_kv_hash shared helper).
    ObservabilityPrims::register_stats_impl(
        "compile:incremental-typecheck-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t auto_invocations = 0;
            std::uint64_t re_inferred = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                auto_invocations =
                    m->incremental_typecheck_auto_invocations_total.load(std::memory_order_relaxed);
                re_inferred =
                    m->incremental_typecheck_re_inferred_total.load(std::memory_order_relaxed);
            }
            const std::uint64_t avg_bp =
                (auto_invocations > 0) ? (re_inferred * 10000u) / auto_invocations : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"auto-invocations-total", make_int(static_cast<std::int64_t>(auto_invocations))},
                {"re-inferred-total", make_int(static_cast<std::int64_t>(re_inferred))},
                {"avg-re-inferred-bp", make_int(static_cast<std::int64_t>(avg_bp))},
            };
            return build_kv_hash(ev, kv);
        });
}

// Issue #909 compile part 47 (orig 3872-3949)
void CompilePrims::register_compile_p47(PrimRegistrar add, Evaluator& ev) {

    // (compile:type-cache-stats) — Issue #412: observability
    // for the type cache generation-counter check. Returns a
    // hash with 4 fields:
    //   - cache-hits-total: lifetime total cache_hits (post-
    //     #412, includes the gen_saved rescues — they're
    //     counted as hits now, not stale)
    //   - cache-misses-total: lifetime total cache_misses
    //   - stale-cache-total: lifetime total stale_cache (post-
    //     #412, only true staleness — false positives
    //     rescued by the gen check no longer count here)
    //   - gen-saved-total: lifetime total cache hits rescued
    //     by the gen check (would have been stale_cache
    //     pre-#412)
    //   - gen-saved-ratio-bp: derived ratio (gen_saved /
    //     (stale + gen_saved) * ::aura::compiler::kBasisPointScale, basis points). 0
    //     when neither counter has been bumped. The key AC
    //     for #412 — higher = more false-positive stale
    //     rejections eliminated.
    ObservabilityPrims::register_stats_impl(
        "compile:type-cache-stats", [&ev](const auto&) -> EvalValue {
            std::uint64_t hits = 0, misses = 0, stale = 0, gen_saved = 0;
            if (ev.compiler_metrics_) {
                auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
                hits = m->typecheck_cache_hits_total.load(std::memory_order_relaxed);
                misses = m->typecheck_cache_misses_total.load(std::memory_order_relaxed);
                stale = m->typecheck_stale_cache_total.load(std::memory_order_relaxed);
                gen_saved = m->typecheck_gen_saved_total.load(std::memory_order_relaxed);
            }
            const std::uint64_t gen_total = stale + gen_saved;
            const std::uint64_t ratio_bp = (gen_total > 0) ? (gen_saved * 10000u) / gen_total : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"cache-hits-total", make_int(static_cast<std::int64_t>(hits))},
                {"cache-misses-total", make_int(static_cast<std::int64_t>(misses))},
                {"stale-cache-total", make_int(static_cast<std::int64_t>(stale))},
                {"gen-saved-total", make_int(static_cast<std::int64_t>(gen_saved))},
                {"gen-saved-ratio-bp", make_int(static_cast<std::int64_t>(ratio_bp))},
            };
            return build_kv_hash(ev, kv);
        });
}

} // namespace aura::compiler::primitives_detail
