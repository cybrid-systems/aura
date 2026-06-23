// evaluator_defuse_index.cpp — P1-a: DefUseIndex + defuse_index_destroy
// aura.compiler.evaluator module partition.

module;

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler {

using EvalValue = types::EvalValue;
using namespace types;

struct DefUseIndex {
    using NodeId = aura::ast::NodeId;
    using SymId = aura::ast::SymId;
    using FlatAST = aura::ast::FlatAST;
    using StringPool = aura::ast::StringPool;
    using NodeTag = aura::ast::NodeTag;
    static constexpr auto INVALID_SYM = aura::ast::INVALID_SYM;

    struct ScopeNode {
        NodeId node = 0;
        std::uint32_t parent = std::uint32_t(-1);
        std::uint32_t first_child = 0;
        std::uint16_t child_count = 0;
        std::uint32_t def_first = 0;
        std::uint16_t def_count = 0;
        std::uint32_t ref_first = 0;
        std::uint16_t ref_count = 0;
        std::uint32_t use_first = 0;
        std::uint32_t use_count = 0;
        bool dirty = false;
        bool tombstoned = false;
    };

    struct SymRef {
        SymId sym = INVALID_SYM;
        std::uint32_t use_start = 0;
        std::uint16_t use_count = 0;
    };

    // Arena data — all flat vectors, no pointers
    std::vector<ScopeNode> scopes_;
    std::vector<SymId> def_syms_;
    std::vector<NodeId> def_nodes_;
    std::vector<SymRef> refs_;
    std::vector<NodeId> uses_;

    // Cross-scope: sym → all scopes that define/reference it
    std::vector<SymId> sym_scopes_keys_;
    std::vector<std::uint32_t> sym_scopes_vals_;
    std::vector<std::uint32_t> sym_to_range_;

    // ── Call-graph index (#10) ─────────────────────────────────
    // callers_of_: SymId → all Call nodes that call this symbol
    // built during build(), enables O(1) query_callers
    std::unordered_map<SymId, std::vector<NodeId>> callers_of_;
    // callee_of_: NodeId → SymId (only for Call nodes)
    // enables O(1) callee lookup from a call site
    std::vector<SymId> callee_of_;

    FlatAST* flat_ = nullptr;
    StringPool* pool_ = nullptr;
    bool built_ = false;
    std::size_t flat_size_at_build_ = 0;

    // ── Per-symbol version (Issue #107 part 5) ──────────────────
    // Tracks which syms have been touched (mutated) since the last
    // index refresh. Each touch bumps global_version_; after a refresh
    // (full build or incremental update_callers_for), the affected
    // syms are removed from stale_syms_. Syms not in stale_syms_ are
    // guaranteed to have fresh callers_of_ / callee_of_ data.
    //
    // Why per-sym and not just one global counter (defuse_version_):
    // - Granular invalidation: a mutation to sym X only invalidates X.
    //   Other syms' cached data stays valid; we can skip their re-scan.
    // - Observability: query:index-stats reports how many syms are
    //   stale, which is useful for cache-hit rate diagnostics.
    // - Forward-compatible: future fine-grained refresh paths (refresh
    //   only stale syms without full flat scan) can use this directly.
    //
    // Note: stale_syms_ is a superset of "syms that changed". After
    // a full build, stale_syms_ is empty. After touch_sym(s), {s} is
    // added. After update_callers_for(S), all s ∈ S are removed.
    std::unordered_set<SymId> stale_syms_;
    std::uint64_t global_version_ = 0;

    // Mark a sym as touched: its callers_of_ / callee_of_ data may
    // now be stale. Bumps global_version_ so is_sym_stale() returns
    // true for this sym until mark_sym_fresh() is called.
    void touch_sym(SymId s) {
        if (s == INVALID_SYM)
            return;
        stale_syms_.insert(s);
        ++global_version_;
    }

    // Touch multiple syms at once. Bumps global_version_ once
    // regardless of |syms| (the bump is a logical "mutation epoch"
    // marker, not a per-sym counter).
    void touch_syms(const std::unordered_set<SymId>& syms) {
        if (syms.empty())
            return;
        for (auto s : syms) {
            if (s != INVALID_SYM)
                stale_syms_.insert(s);
        }
        ++global_version_;
    }

    // Check if a sym's data is stale.
    bool is_sym_stale(SymId s) const { return stale_syms_.count(s) > 0; }

    // Mark a sym as fresh (its callers_of_ / callee_of_ data is
    // up-to-date with the current flat).
    void mark_sym_fresh(SymId s) { stale_syms_.erase(s); }

    // Mark a set of syms as fresh.
    void mark_syms_fresh(const std::unordered_set<SymId>& syms) {
        for (auto s : syms)
            stale_syms_.erase(s);
    }

    // Mark all syms as fresh (called after a full build).
    void mark_all_fresh() { stale_syms_.clear(); }

    // Stats accessors.
    std::size_t stale_count() const { return stale_syms_.size(); }
    std::uint64_t current_version() const { return global_version_; }

    void destroy() {
        scopes_.clear();
        def_syms_.clear();
        def_nodes_.clear();
        refs_.clear();
        uses_.clear();
        sym_scopes_keys_.clear();
        sym_scopes_vals_.clear();
        sym_to_range_.clear();
        callers_of_.clear();
        callee_of_.clear();
        stale_syms_.clear();
        global_version_ = 0;
        flat_ = nullptr;
        pool_ = nullptr;
        built_ = false;
    }

    // ── Build from scratch ──────────────────────────────────────
    // Single-pass: walk AST nodes 0..N-1, detect scope boundaries,
    // collect defs, build scope tree, then collect uses per scope.
    void build(FlatAST& flat, StringPool& pool) {
        destroy();
        flat_ = &flat;
        pool_ = &pool;
        flat_size_at_build_ = flat.size();

        // Pre-allocate
        def_syms_.reserve(flat.size() / 4);
        def_nodes_.reserve(flat.size() / 4);
        uses_.reserve(flat.size() / 2);
        refs_.reserve(flat.size() / 4);

        // Root scope (module-level: node 0)
        scopes_.push_back({});
        scopes_.back().node = 0;
        scopes_.back().dirty = false;

        // Pass 1: walk all nodes, build scope tree + collect defs
        // Use explicit depth-first traversal, NOT scan-by-NodeId
        // because children may not be contiguous.
        struct Frame {
            NodeId node_id;
            std::uint32_t scope_idx;
            std::size_t child_idx; // which child we're processing
        };
        std::vector<Frame> stack;
        stack.push_back({flat.root, 0, 0});

        while (!stack.empty()) {
            auto& f = stack.back();
            auto v = flat.get(f.node_id);

            // First visit: check if this node creates a scope
            if (f.child_idx == 0) {
                bool is_scope_creator = false;
                switch (v.tag) {
                    case NodeTag::Define:
                    case NodeTag::Lambda:
                    case NodeTag::Let:
                    case NodeTag::LetRec:
                    case NodeTag::Begin:
                        is_scope_creator = true;
                        break;
                    default:
                        break;
                }

                if (is_scope_creator && f.node_id != flat.root) {
                    // Create new scope (except for root which already has scope 0)
                    auto scope_idx = scopes_.size();
                    ScopeNode sn;
                    sn.node = f.node_id;
                    sn.parent = f.scope_idx;
                    sn.dirty = false;
                    scopes_.push_back(sn);

                    // Link into parent
                    auto& parent = scopes_[f.scope_idx];
                    if (parent.child_count == 0)
                        parent.first_child = scope_idx;
                    parent.child_count++;

                    // Collect defs for this scope
                    auto& sn2 = scopes_.back();
                    sn2.def_first = def_syms_.size();
                    switch (v.tag) {
                        case NodeTag::Define:
                            def_syms_.push_back(v.sym_id);
                            def_nodes_.push_back(f.node_id);
                            break;
                        case NodeTag::Lambda:
                            for (auto pid : v.params) {
                                def_syms_.push_back(pid);
                                def_nodes_.push_back(f.node_id);
                            }
                            break;
                        case NodeTag::Let:
                        case NodeTag::LetRec:
                            def_syms_.push_back(v.sym_id);
                            def_nodes_.push_back(f.node_id);
                            break;
                        default:
                            break;
                    }
                    sn2.def_count = def_syms_.size() - sn2.def_first;

                    // Update frame scope
                    f.scope_idx = scope_idx;
                }
            }

            // Process children
            if (f.child_idx < v.children.size()) {
                auto child = v.child(f.child_idx);
                auto child_scope = f.scope_idx;

                // Skip scope-creating children (they create their own scope)
                // But still process them as sub-frames
                auto cv = flat.get(child);
                f.child_idx++;
                stack.push_back({child, child_scope, 0});
            } else {
                stack.pop_back();
                // When returning from a scope-creating child, the parent scope stays
            }
        }

        // Pass 2: collect uses per scope
        // Walk all Variable nodes, associate each with the innermost scope
        // that could define it (or skip if unbound)
        // For simplicity: associate each Variable with the scope it belongs to
        collect_uses(flat);

        // Pass 3: add any unfound defs from full scan (covers edge cases)
        // This ensures top-level defines and lets are always indexed
        for (NodeId sid = 0; sid < flat.size(); ++sid) {
            auto sv = flat.get(sid);
            aura::ast::SymId def_sym = aura::ast::INVALID_SYM;

            // Check for define/let/letrec that might not be in any scope
            if (sv.tag == NodeTag::Define || sv.tag == NodeTag::Let || sv.tag == NodeTag::LetRec) {
                def_sym = sv.sym_id;
            } else if (sv.tag == NodeTag::Lambda && sv.params.size() > 0) {
                // For lambdas at top level, add all params
            }

            if (def_sym != aura::ast::INVALID_SYM) {
                // Find which scope this node belongs to
                std::uint32_t found_scope = 0; // default: root scope
                for (std::uint32_t si = 0; si < scopes_.size(); ++si) {
                    auto& sn = scopes_[si];
                    if (sn.node == sid) {
                        found_scope = si;
                        break;
                    }
                }

                // Check if this sym is already def'd in this scope
                auto& sn = scopes_[found_scope];
                bool exists = false;
                for (std::uint16_t d = 0; d < sn.def_count; ++d) {
                    if (def_syms_[sn.def_first + d] == def_sym) {
                        exists = true;
                        break;
                    }
                }

                if (!exists) {
                    // Insert def at the end of this scope's defs
                    // Need to shift: append new def, update scope's def_range
                    // For root scope, just append
                    auto old_def_first = sn.def_first;
                    auto old_def_count = sn.def_count;
                    def_syms_.push_back(def_sym);
                    def_nodes_.push_back(sid);
                    sn.def_first = def_syms_.size() - 1;
                    sn.def_count = 1 + old_def_count;
                }
            }
        }

        // Pass 4: build cross-scope sym index
        build_sym_index();

        // Pass 5: build call-graph index (#10)
        // Walk all Call nodes, record callers_of_ and callee_of_
        build_call_graph(flat);

        // All syms are now fresh after a full build. (Issue #107 part 5)
        // global_version_ is kept monotonic (never reset); staleness
        // is tracked by stale_syms_ membership, not by version compare.
        mark_all_fresh();

        built_ = true;
    }

    // ── Collect uses: walk all Variable nodes, group by scope ────
    void collect_uses(FlatAST& flat) {
        // Map: node_id → scope_idx
        std::unordered_map<NodeId, std::uint32_t> node_to_scope;
        node_to_scope.reserve(flat.size());

        // Build node-to-scope mapping via DFS
        struct Frame {
            NodeId nid;
            std::uint32_t scope_idx;
            std::size_t child_idx;
        };
        std::vector<Frame> stack;
        stack.push_back({flat.root, 0, 0});

        while (!stack.empty()) {
            auto& f = stack.back();
            auto v = flat.get(f.nid);

            if (f.child_idx == 0) {
                // First visit: determine scope
                // Check parent scope
                auto this_scope = f.scope_idx;

                // If this node creates a scope, find its scope index
                bool found = false;
                for (std::size_t si = scopes_.size(); si > 0; --si) {
                    auto& sn = scopes_[si - 1];
                    if (sn.node == f.nid && !sn.tombstoned) {
                        this_scope = si - 1;
                        found = true;
                        break;
                    }
                }
                node_to_scope[f.nid] = this_scope;

                if (f.child_idx == 0) {
                    f.scope_idx = this_scope;
                }
            }

            if (f.child_idx < v.children.size()) {
                auto child = v.child(f.child_idx);
                f.child_idx++;
                stack.push_back({child, f.scope_idx, 0});
            } else {
                stack.pop_back();
            }
        }

        // Now collect Variables grouped by scope
        // Group by scope: scope_idx → {sym → [node_ids]}
        struct ScopeVarGroup {
            std::unordered_map<SymId, std::vector<NodeId>> vars;
        };
        std::unordered_map<std::uint32_t, ScopeVarGroup> scope_vars;

        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Variable) {
                auto scope_it = node_to_scope.find(id);
                if (scope_it != node_to_scope.end()) {
                    scope_vars[scope_it->second].vars[v.sym_id].push_back(id);
                }
            }
        }

        // Build refs_ and uses_ from scope_vars
        for (std::uint32_t si = 0; si < scopes_.size(); ++si) {
            auto& sn = scopes_[si];
            sn.ref_first = refs_.size();
            sn.use_first = uses_.size();

            auto sv_it = scope_vars.find(si);
            if (sv_it != scope_vars.end()) {
                for (auto& [sym, nodes] : sv_it->second.vars) {
                    SymRef sr;
                    sr.sym = sym;
                    sr.use_start = uses_.size();
                    sr.use_count = static_cast<std::uint16_t>(nodes.size());
                    for (auto nid : nodes)
                        uses_.push_back(nid);
                    refs_.push_back(sr);
                }
            }

            sn.ref_count = static_cast<std::uint16_t>(refs_.size() - sn.ref_first);
            sn.use_count = uses_.size() - sn.use_first;
        }
    }

    // ── Build sym → scopes index ────────────────────────────────
    void build_sym_index() {
        SymId max_sym = 0;
        for (auto s : def_syms_)
            if (s != INVALID_SYM && s > max_sym)
                max_sym = s;
        for (auto& r : refs_)
            if (r.sym != INVALID_SYM && r.sym > max_sym)
                max_sym = r.sym;

        sym_to_range_.resize(max_sym + 1, 0);

        struct Entry {
            SymId sym;
            std::uint32_t scope_idx;
            bool is_def;
            std::uint32_t local_idx;
        };
        std::unordered_map<uint32_t, std::vector<Entry>> entries_by_sym;

        for (std::uint32_t si = 0; si < scopes_.size(); ++si) {
            auto& sn = scopes_[si];
            for (std::uint16_t d = 0; d < sn.def_count; ++d) {
                auto sym = def_syms_[sn.def_first + d];
                entries_by_sym[sym].push_back({sym, si, true, sn.def_first + d});
            }
            for (std::uint16_t r = 0; r < sn.ref_count; ++r) {
                auto& ref = refs_[sn.ref_first + r];
                entries_by_sym[ref.sym].push_back({ref.sym, si, false, sn.ref_first + r});
            }
        }

        sym_scopes_keys_.clear();
        sym_scopes_vals_.clear();

        for (auto& [sym, entries] : entries_by_sym) {
            if (sym > max_sym)
                continue;
            sym_to_range_[sym] = (sym_scopes_vals_.size() << 16) | (uint32_t)entries.size();
            for (auto& e : entries) {
                sym_scopes_keys_.push_back(sym);
                sym_scopes_vals_.push_back((e.scope_idx << 1) | (e.is_def ? 1u : 0u));
            }
        }
    }

    // ── Build call-graph index (#10) ────────────────────────────
    // Populates callers_of_ and callee_of_ from all Call nodes.
    void build_call_graph(FlatAST& flat) {
        callers_of_.clear();
        callee_of_.resize(flat.size(), INVALID_SYM);
        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Call && !v.children.empty()) {
                auto callee = flat.get(v.child(0));
                if (callee.tag == NodeTag::Variable && callee.sym_id != INVALID_SYM) {
                    callers_of_[callee.sym_id].push_back(id);
                    callee_of_[id] = callee.sym_id;
                }
            }
        }
    }

    // ── Query: def-use for a symbol ─────────────────────────────
    struct DefUseResult {
        std::vector<NodeId> defs;
        std::vector<NodeId> uses;
    };

    DefUseResult query_def_use(SymId sym) {
        DefUseResult r;
        if (sym >= sym_to_range_.size())
            return r;

        auto packed = sym_to_range_[sym];
        if (packed == 0)
            return r;

        auto start = packed >> 16;
        auto count = packed & 0xFFFF;

        for (std::uint32_t i = start; i < start + count; ++i) {
            auto val = sym_scopes_vals_[i];
            auto scope_idx = val >> 1;
            if (val & 1) {
                // is_def
                auto& sn = scopes_[scope_idx];
                for (std::uint16_t d = 0; d < sn.def_count; ++d) {
                    if (def_syms_[sn.def_first + d] == sym)
                        r.defs.push_back(def_nodes_[sn.def_first + d]);
                }
            } else {
                // is_ref — collect use nodes
                auto& sn = scopes_[scope_idx];
                for (std::uint16_t ri = 0; ri < sn.ref_count; ++ri) {
                    auto& ref = refs_[sn.ref_first + ri];
                    if (ref.sym == sym) {
                        for (std::uint16_t u = 0; u < ref.use_count; ++u)
                            r.uses.push_back(uses_[ref.use_start + u]);
                    }
                }
            }
        }
        return r;
    }

    // ── Query: caller nodes for a symbol ────────────────────────
    // O(1) callee lookup: which symbol does a Call node invoke?
    // Returns INVALID_SYM if not a call or not indexed.
    SymId query_callee(NodeId node) const {
        if (node < callee_of_.size())
            return callee_of_[node];
        return INVALID_SYM;
    }

    // O(1) caller query using callers_of_ index (built during build())
    // Fallback: if index not available (build_call_graph wasn't run), do O(N) scan
    std::vector<NodeId> query_callers(SymId sym, FlatAST& flat) {
        // Try indexed path first
        if (!callers_of_.empty()) {
            auto it = callers_of_.find(sym);
            if (it != callers_of_.end())
                return it->second;
            return {};
        }
        // Fallback: O(N) scan for unindexed state
        std::vector<NodeId> callers;
        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Call && !v.children.empty()) {
                auto callee = flat.get(v.child(0));
                if (callee.tag == NodeTag::Variable && callee.sym_id == sym)
                    callers.push_back(id);
            }
        }
        return callers;
    }

    // ── Mark scope containing a node as dirty ───────────────────
    void mark_dirty(NodeId node) {
        if (!built_ || scopes_.empty())
            return;
        for (auto& sn : scopes_) {
            if (sn.tombstoned)
                continue;
            if (sn.node == node) {
                mark_dirty_up(sn);
                return;
            }
        }
        for (auto& sn : scopes_)
            sn.dirty = true;
    }

    void mark_dirty_up(ScopeNode& sn) {
        sn.dirty = true;
        if (sn.parent < scopes_.size() && !scopes_[sn.parent].tombstoned)
            mark_dirty_up(scopes_[sn.parent]);
    }

    // ── Incremental rebuild ─────────────────────────────────────
    bool rebuild_dirty(FlatAST& flat, StringPool& pool) {
        if (!built_) {
            build(flat, pool);
            return true;
        }
        if (flat_ != &flat || flat.size() != flat_size_at_build_) {
            build(flat, pool);
            return true;
        }
        bool any_dirty = false;
        for (auto& sn : scopes_) {
            if (sn.dirty) {
                any_dirty = true;
                break;
            }
        }
        if (!any_dirty)
            return false;
        build(flat, pool);
        return true;
    }

    // ── Incremental: update callers_of_ for specific syms ─────
    // Used after mutations that only modify existing nodes
    // (rebind/set-body/replace-pattern) without adding new nodes.
    // Full flat scan still needed but scope tree + defs/uses preserved.
    //
    // After this call, all syms in `affected_syms` have fresh
    // callers_of_ / callee_of_ data. We mark them as fresh in the
    // per-sym version tracker (Issue #107 part 5) so subsequent
    // ensure_defuse() calls don't redundantly re-touch them.
    void update_callers_for(FlatAST& flat, const std::unordered_set<SymId>& affected_syms) {
        if (!built_ || affected_syms.empty())
            return;
        // Clear old callers entries for affected syms
        for (auto sym : affected_syms)
            callers_of_.erase(sym);
        // Reset callee_of_ for call nodes that referenced affected syms
        for (NodeId id = 0; id < callee_of_.size(); ++id) {
            if (callee_of_[id] != INVALID_SYM && affected_syms.count(callee_of_[id]))
                callee_of_[id] = INVALID_SYM;
        }
        // Full scan to find new Call nodes referencing affected syms
        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Call && !v.children.empty()) {
                auto callee = flat.get(v.child(0));
                if (callee.tag == NodeTag::Variable && callee.sym_id != INVALID_SYM &&
                    affected_syms.count(callee.sym_id)) {
                    callers_of_[callee.sym_id].push_back(id);
                    callee_of_[id] = callee.sym_id;
                }
            }
        }
        // Mark refreshed syms as fresh. (Issue #107 part 5)
        // We erase from stale_syms_ even if they weren't stale before
        // (idempotent); the data is now up-to-date either way.
        mark_syms_fresh(affected_syms);
    }
};

// ── DefUseIndex ownership helper (ASAN fix #107 leak) ───────────
// Free a DefUseIndex held in a void* slot and null the slot. Defined
// after the struct body so DefUseIndex's destructor is visible (a
// forward-declared type's destructor is not, so `delete static_cast
// <DefUseIndex*>(...)` won't compile from sites before the struct).
// The function is small and the call sites pass `&defuse_index_`,
// which is the same idiom as `defuse_touch_fn_` and friends.
//
// Note: we can't use std::unique_ptr<DefUseIndex> in the header
// because DefUseIndex is a TU-local type (defined in this TU;
// only). The void* slot + this helper is the PIMPL-shaped equivalent.
void defuse_index_destroy(void** slot) {
    if (!slot || !*slot)
        return;
    delete static_cast<DefUseIndex*>(*slot);
    *slot = nullptr;
}


void Evaluator::install_defuse_subsystem() {
    // ═══════════════════════════════════════════════════════════════
    // P9: Def-Use Analysis (P1 — scope-level cached)
    // ═══════════════════════════════════════════════════════════════

    // ── 依赖图查询回调注册 ─────────────────────────────────────
    // 在 def-use 索引中注册依赖图查询函数，供 mutation 原语
    // (mutate:rebind / set-body) 在变更前查询调用者节点。
    // 定义在这里（DefUseIndex 完整类型已知后），绕开前向声明问题。
    dep_caller_fn_ = [](void* idx_ptr, aura::ast::SymId sym) -> std::vector<aura::ast::NodeId> {
        if (!idx_ptr)
            return {};
        auto* idx = static_cast<DefUseIndex*>(idx_ptr);
        auto result = idx->query_def_use(sym);
        return std::move(result.uses);
    };

    // ── Per-sym version touch callback (#107 part 5) ───────────
    // Registers a callback that mutations can use to mark a sym as
    // stale in the DefUseIndex. Same forward-decl workaround as
    // dep_caller_fn_. When defuse_index_ is null (no index yet), the
    // callback is a no-op; the next ensure_defuse() will build from
    // scratch anyway.
    defuse_touch_fn_ = [](void* idx_ptr, aura::ast::SymId sym) {
        if (!idx_ptr)
            return;
        auto* idx = static_cast<DefUseIndex*>(idx_ptr);
        idx->touch_sym(sym);
    };

    // Helper: get or rebuild the def-use index
    // Tracks defuse_version_ to detect mutations since last build.
    // (#10) Tracks rebuild count and clears affected_syms_ after rebuild.
    // (#107 part 5) Per-sym version: DefUseIndex itself tracks which
    // syms are stale (DefUseIndex::stale_syms_). The mutation paths
    // that report affected_syms (mutate:rebind / set-body) also call
    // defuse_touch_fn_ to mark the sym stale in the index. The
    // staleness set is used for observability (query:index-stats)
    // and for the update_callers_for() path to know which syms need
    // fresh data. We deliberately do NOT short-circuit ensure_defuse
    // on staleness: if a mutation reports an affected sym, we must
    // refresh it, even if the per-sym version wasn't bumped (defense
    // in depth: the affected_syms_ list is the authoritative source
    // for "this sym needs re-indexing", and the per-sym version is
    // a co-located observation).
    auto ensure_defuse = [this]() -> DefUseIndex* {
        if (!workspace_flat_ || !workspace_pool_)
            return nullptr;
        auto idx = static_cast<DefUseIndex*>(defuse_index_);
        if (!idx) {
            idx = new DefUseIndex();
            defuse_index_ = idx;
            idx->build(*workspace_flat_, *workspace_pool_);
            defuse_version_.store(1, std::memory_order_relaxed);
            defuse_rebuild_count_++;
            defuse_affected_syms_.clear();
            return idx;
        }

        // Collect affected syms since last ensure_defuse
        std::unordered_set<aura::ast::SymId> affected_sym_ids;
        if (!defuse_affected_syms_.empty()) {
            for (auto& name : defuse_affected_syms_) {
                auto sym = workspace_pool_->intern(name);
                if (sym != aura::ast::INVALID_SYM)
                    affected_sym_ids.insert(sym);
            }
        }
        defuse_affected_syms_.clear();

        // Incremental path: only rebuild callers_of_ for affected syms
        // when flat size hasn't changed (mutations that modify existing nodes).
        if (!affected_sym_ids.empty()) {
            if (workspace_flat_->size() == idx->flat_size_at_build_) {
                idx->update_callers_for(*workspace_flat_, affected_sym_ids);
                defuse_version_.store(1, std::memory_order_relaxed);
                return idx;
            }
        }

        // Fallback: full rebuild (flat size changed or many affected syms)
        idx->build(*workspace_flat_, *workspace_pool_);
        defuse_version_.store(1, std::memory_order_relaxed);
        defuse_rebuild_count_++;
        return idx;
    };

    // Helper: build Aura result list from NodeIds
    auto nodes_to_list = [this](std::span<const DefUseIndex::NodeId> nodes) -> EvalValue {
        EvalValue list = make_void();
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            auto pid = pairs_.size();
            pairs_.push_back({make_int(static_cast<std::int64_t>(*it)), list});
            list = make_pair(pid);
        }
        return list;
    };

    auto def_use_pair = [this, nodes_to_list](DefUseIndex* idx, aura::ast::SymId sym) -> EvalValue {
        auto result = idx->query_def_use(sym);
        auto def_list = nodes_to_list(result.defs);
        auto use_list = nodes_to_list(result.uses);
        auto result_pid = pairs_.size();
        pairs_.push_back({def_list, use_list});
        return make_pair(result_pid);
    };

    primitives_detail::register_defuse_query_primitives(
        prim_registrar(),
        workspace_mtx_, workspace_flat_, workspace_pool_, string_heap_, ensure_defuse,
        [def_use_pair](void* idx, aura::ast::SymId sym) {
            return def_use_pair(static_cast<DefUseIndex*>(idx), sym);
        },
        [this, nodes_to_list, def_use_pair](void* idx, aura::ast::NodeId target) -> EvalValue {
            auto& flat = *workspace_flat_;
            auto v = flat.get(target);
            aura::ast::SymId defined_sym = aura::ast::INVALID_SYM;
            switch (v.tag) {
                case aura::ast::NodeTag::Define:
                case aura::ast::NodeTag::Let:
                case aura::ast::NodeTag::LetRec:
                    defined_sym = v.sym_id;
                    break;
                default:
                    return make_merr("type-error", "node " + std::to_string(target) +
                                                       " is not a definition node");
            }
            if (defined_sym == aura::ast::INVALID_SYM)
                return make_merr("internal", "definition node has invalid symbol id");
            return def_use_pair(static_cast<DefUseIndex*>(idx), defined_sym);
        },
        [this, nodes_to_list](void* idx, aura::ast::SymId sym) -> EvalValue {
            auto* du_idx = static_cast<DefUseIndex*>(idx);
            auto& flat = *workspace_flat_;
            auto duo = du_idx->query_def_use(sym);
            auto callers = du_idx->query_callers(sym, flat);
            auto def_list = nodes_to_list(duo.defs);
            auto use_list = nodes_to_list(duo.uses);
            auto caller_list = nodes_to_list(callers);
            auto c1 = pairs_.size();
            pairs_.push_back({caller_list, make_void()});
            auto c2 = pairs_.size();
            pairs_.push_back({use_list, make_pair(c1)});
            auto c3 = pairs_.size();
            pairs_.push_back({def_list, make_pair(c2)});
            return make_pair(c3);
        },
        [this](void* idx) -> EvalValue {
            auto* du_idx = static_cast<DefUseIndex*>(idx);
            du_idx->build(*workspace_flat_, *workspace_pool_);
            defuse_version_.store(1, std::memory_order_relaxed);
            defuse_rebuild_count_++;
            defuse_affected_syms_.clear();
            return make_int(1);
        },
        [this](void* idx) -> EvalValue {
            auto* du_idx = static_cast<DefUseIndex*>(idx);
            auto& flat = *workspace_flat_;
            auto make_kv = [&](std::string_view k, std::int64_t v) -> types::EvalValue {
                auto kv_ref = make_pair(pairs_.size());
                auto k_sym = string_heap_.size();
                string_heap_.push_back(std::string(k));
                types::EvalValue car = make_string(k_sym);
                types::EvalValue cdr = make_int(v);
                pairs_.push_back(Pair{car, cdr});
                return kv_ref;
            };
            auto stats = make_void();
            auto push_kv = [&](std::string_view k, std::int64_t v) {
                auto kv_ref = make_kv(k, v);
                auto new_ref = make_pair(pairs_.size());
                pairs_.push_back(Pair{kv_ref, stats});
                stats = new_ref;
            };
            push_kv("nodes", flat.size());
            push_kv("scopes", du_idx->scopes_.size());
            push_kv("def-syms", du_idx->def_syms_.size());
            push_kv("refs", du_idx->refs_.size());
            push_kv("callers", du_idx->callers_of_.size());
            push_kv("rebuilds", static_cast<std::int64_t>(defuse_rebuild_count_));
            push_kv("stale-syms", static_cast<std::int64_t>(du_idx->stale_count()));
            push_kv("defuse-version", static_cast<std::int64_t>(du_idx->current_version()));
            return stats;
        },
        [this](const std::string& k, const std::string& m) { return make_merr(k, m); });

    primitives_detail::register_ast_primitives(
        prim_registrar(),
        *this, [this]() { defuse_index_destroy(&defuse_index_); },
        [this]() -> std::optional<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> {
            auto* idx = static_cast<DefUseIndex*>(defuse_index_);
            if (!idx || !idx->built_)
                return std::nullopt;
            return std::make_tuple(static_cast<std::uint64_t>(idx->scopes_.size()),
                                   static_cast<std::uint64_t>(idx->def_syms_.size()),
                                   static_cast<std::uint64_t>(idx->uses_.size()));
        });

    }
} // namespace aura::compiler
