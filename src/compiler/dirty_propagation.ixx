// dirty_propagation.ixx — Issue #1206 Phase 1 scaffold + Issue #1575 Phase 2:
// automatic BFS cascade, IR dirty bridges, DirtyAwarePass integration hooks.

module;

#include "core/transparent_string_hash.hh" // #2247 dual-graph string maps

export module aura.compiler.dirty_propagation;

import std;

export namespace aura::compiler::dirty {

inline constexpr int kDirtyPropagationPhase = 2; // #1575: cascade + bridges

using NodeId = std::uint32_t;

// ── Metrics (#1575 / #2063 / #2106) ────────────────────────────
// Module-level atomics so Agent / tests can observe cascade health
// without going through CompilerService.
//
// Issue #2063: cascade_mark_dirty bumps dirty_skip_subtree on
// summary-dirty early-exit. Issue #2106: callers / CompilerService
// register g_cascade_skip_subtree_metrics (CompilerMetrics::
// cascade_skip_subtree_total); flush_dirty_skip_subtree_to_metrics
// exchanges the file-scope counter into that sink so nested
// cascades never double-count (exchange zeros the source).
inline std::atomic<std::uint64_t> dirty_skip_subtree{0};
inline std::atomic<std::uint64_t> dirty_propagation_bfs_hits{0};
inline std::atomic<std::uint64_t> manual_propagate_deprecated_count{0};
// Running sum of BFS depths + sample count → dirty_cascade_depth_avg.
inline std::atomic<std::uint64_t> dirty_cascade_depth_sum{0};
inline std::atomic<std::uint64_t> dirty_cascade_depth_samples{0};
inline std::atomic<std::uint64_t> dirty_cascade_nodes_marked_total{0};
inline std::atomic<std::uint64_t> dirty_sync_from_ir_total{0};
inline std::atomic<std::uint64_t> dirty_push_to_ir_total{0};

// Issue #2191: type affected-subtree cone mirrored into DepGraph cascade.
// type_dirty_cone_mirrored_total — AST NodeIds pushed as cascade roots.
// type_ir_cone_union_size_{sum,samples} — avg |type ∪ IR dirty| after mirror.
inline std::atomic<std::uint64_t> type_dirty_cone_mirrored_total{0};
inline std::atomic<std::uint64_t> type_ir_cone_union_size_sum{0};
inline std::atomic<std::uint64_t> type_ir_cone_union_samples{0};

// Issue #2106: optional sink → CompilerMetrics::cascade_skip_subtree_total.
// Set by CompilerService ctor (or tests); null when no service is live.
inline std::atomic<std::uint64_t>* g_cascade_skip_subtree_metrics = nullptr;

inline void set_cascade_skip_subtree_metrics(std::atomic<std::uint64_t>* sink) noexcept {
    g_cascade_skip_subtree_metrics = sink;
}

// Exchange file-scope dirty_skip_subtree into the metrics sink.
// Returns the drained count (0 if sink is null — leaves file-scope
// intact so unit tests without a service can still load it).
// Exchange semantics: nested cascade_mark_dirty / cascade_mark_dirty_many
// must not double-count the same skips into metrics.
[[nodiscard]] inline std::uint64_t flush_dirty_skip_subtree_to_metrics() noexcept {
    auto* sink = g_cascade_skip_subtree_metrics;
    if (!sink)
        return 0;
    const auto n = dirty_skip_subtree.exchange(0, std::memory_order_relaxed);
    if (n)
        sink->fetch_add(n, std::memory_order_relaxed);
    return n;
}

// Agent-visible total: metrics sink (if set) + any unflushed file-scope.
[[nodiscard]] inline std::uint64_t cascade_skip_subtree_visible() noexcept {
    std::uint64_t n = dirty_skip_subtree.load(std::memory_order_relaxed);
    if (g_cascade_skip_subtree_metrics)
        n += g_cascade_skip_subtree_metrics->load(std::memory_order_relaxed);
    return n;
}

struct DirtyPropagationStats {
    std::uint64_t marks = 0;
    std::uint64_t propagations = 0;
    std::uint64_t queries = 0;
    std::uint64_t clean_hits = 0;
    std::uint64_t bfs_cascades = 0;
    std::uint64_t bfs_nodes_marked = 0;
};

inline DirtyPropagationStats g_dirty_propagation_stats{};

[[nodiscard]] inline double dirty_cascade_depth_avg() noexcept {
    const auto n = dirty_cascade_depth_samples.load(std::memory_order_relaxed);
    if (n == 0)
        return 0.0;
    const auto s = dirty_cascade_depth_sum.load(std::memory_order_relaxed);
    return static_cast<double>(s) / static_cast<double>(n);
}

// ── DepGraph: adjacency list for automatic cascade ─────────────
// Edge meaning: dirty propagates from `from` to each dependent in
// adj[from] (BFS along called_by / dataflow dependents).
//
// Independent of CompilerService::dep_graph_ (string-keyed function
// names). Tests and passes can build a lightweight NodeId graph;
// service may mirror function-level edges via encode_fn_node().
struct DepGraph {
    std::unordered_map<NodeId, std::vector<NodeId>> adj;

    void add_edge(NodeId from, NodeId to) {
        auto& v = adj[from];
        // Dedup cheap path for small fan-out (typical nested lambda).
        if (std::find(v.begin(), v.end(), to) == v.end())
            v.push_back(to);
    }

    void add_edges(NodeId from, std::initializer_list<NodeId> tos) {
        for (NodeId t : tos)
            add_edge(from, t);
    }

    void clear() { adj.clear(); }

    [[nodiscard]] bool empty() const noexcept { return adj.empty(); }

    [[nodiscard]] std::size_t edge_count() const noexcept {
        std::size_t n = 0;
        for (const auto& [_, v] : adj)
            n += v.size();
        return n;
    }

    // Outgoing dependents (may be empty).
    [[nodiscard]] const std::vector<NodeId>* dependents(NodeId id) const {
        auto it = adj.find(id);
        if (it == adj.end())
            return nullptr;
        return &it->second;
    }
};

// ── DirtySet: dense bitset + sparse set for tagged hybrid NodeIds ─
//
// Issue #2110/#2187/#2191 encode fn / block-dep / ast-dep nodes with
// high tag bits (e.g. encode_fn_node(1) → 0x80000001). Using those as
// dense bitset indices would allocate ~2^31 bits (~256 MiB) and spend
// seconds in memset / dirty_nodes() scans — the root of CI "hangs" on
// every mutate:rebind (mark_define_dirty → hybrid_node_cascade_).
//
// Dense path: low AST / local-block ids (no high tags, < kMaxDenseSize).
// Sparse path: any tagged hybrid id or id >= kMaxDenseSize.
struct DirtySet {
    std::vector<bool> bits;
    std::unordered_set<NodeId> sparse;

    // Cap dense growth (~128 KiB for vector<bool>). Above this, mark()
    // uses sparse storage so a single bad id cannot multi-GB allocate.
    static constexpr std::size_t kMaxDenseSize = 1u << 20; // 1M nodes

    // bit31=fn, bit30=block-dep, bit29=ast-dep (see encode_* below).
    static constexpr NodeId kHybridTagMask = 0xE0000000u;

    [[nodiscard]] static bool uses_sparse(NodeId id) noexcept {
        return (id & kHybridTagMask) != 0 || static_cast<std::size_t>(id) >= kMaxDenseSize;
    }

    void ensure(std::size_t n) {
        if (n > kMaxDenseSize)
            return; // refuse multi-GB growth; caller should use sparse
        if (bits.size() < n)
            bits.resize(n, false);
    }

    void mark(NodeId id) {
        if (uses_sparse(id)) {
            sparse.insert(id);
            ++g_dirty_propagation_stats.marks;
            return;
        }
        ensure(static_cast<std::size_t>(id) + 1);
        bits[id] = true;
        ++g_dirty_propagation_stats.marks;
    }

    // Issue #1575: non-stats peek for bridges / cascade internals.
    [[nodiscard]] bool is_dirty(NodeId id) const noexcept {
        if (uses_sparse(id))
            return sparse.contains(id);
        return id < bits.size() && bits[id];
    }

    // Issue #1206 pairwise API — kept for backward compat.
    // Issue #1575: counts as deprecated manual use; prefer cascade_mark_dirty.
    // Historical semantics: set target dirty when from dirty; only bump
    // propagations (not marks).
    [[deprecated("Issue #1575: prefer cascade_mark_dirty / propagate_closure with DepGraph")]]
    void propagate(NodeId from, NodeId to) {
        manual_propagate_deprecated_count.fetch_add(1, std::memory_order_relaxed);
        if (!is_dirty(from))
            return;
        if (uses_sparse(to)) {
            sparse.insert(to);
        } else {
            ensure(static_cast<std::size_t>(to) + 1);
            bits[to] = true;
        }
        ++g_dirty_propagation_stats.propagations;
    }

    // Non-deprecated internal pairwise used by cascade (does not bump
    // manual_propagate_deprecated_count).
    void propagate_edge(NodeId from, NodeId to) {
        if (!is_dirty(from))
            return;
        if (uses_sparse(to)) {
            sparse.insert(to);
        } else {
            ensure(static_cast<std::size_t>(to) + 1);
            bits[to] = true;
        }
        ++g_dirty_propagation_stats.propagations;
    }

    [[nodiscard]] bool query(NodeId id) {
        ++g_dirty_propagation_stats.queries;
        if (!is_dirty(id)) {
            ++g_dirty_propagation_stats.clean_hits;
            return false;
        }
        return true;
    }

    void clear() {
        bits.assign(bits.size(), false);
        sparse.clear();
    }

    [[nodiscard]] std::size_t dirty_count() const noexcept {
        std::size_t n = sparse.size();
        for (std::size_t i = 0; i < bits.size(); ++i)
            if (bits[i])
                ++n;
        return n;
    }

    // Collect all currently-dirty node ids (for tests / dashboards).
    // O(|dense dirty| + |sparse|) — never O(max NodeId).
    [[nodiscard]] std::vector<NodeId> dirty_nodes() const {
        std::vector<NodeId> out;
        out.reserve(sparse.size() + 16);
        for (std::size_t i = 0; i < bits.size(); ++i)
            if (bits[i])
                out.push_back(static_cast<NodeId>(i));
        for (NodeId id : sparse)
            out.push_back(id);
        return out;
    }
};

inline DirtySet g_global_dirty{};

// Optional pipeline dep graph for DirtyAwarePass auto-cascade (#1575 AC2).
inline const DepGraph* g_pipeline_dep_graph = nullptr;
inline thread_local std::vector<NodeId> t_pipeline_cascade_roots{};

inline void set_pipeline_dep_graph(const DepGraph* g) noexcept {
    g_pipeline_dep_graph = g;
}

[[nodiscard]] inline const DepGraph* pipeline_dep_graph() noexcept {
    return g_pipeline_dep_graph;
}

inline void note_pipeline_cascade_root(NodeId root) {
    t_pipeline_cascade_roots.push_back(root);
}

inline void clear_pipeline_cascade_roots() noexcept {
    t_pipeline_cascade_roots.clear();
}

// ── BFS cascade (#1575 AC1) ────────────────────────────────────
// Mark root dirty, then BFS along DepGraph edges, marking every
// reachable dependent. Returns number of newly-marked nodes
// (including root if it was clean). Depth = max BFS level.
// Supports instruction-level nodes: just put inst ids in the graph.
inline std::size_t cascade_mark_dirty(DirtySet& set, NodeId root, const DepGraph& g) {
    dirty_propagation_bfs_hits.fetch_add(1, std::memory_order_relaxed);
    ++g_dirty_propagation_stats.bfs_cascades;

    const bool root_was_clean = !set.is_dirty(root);
    set.mark(root);

    std::queue<std::pair<NodeId, std::uint32_t>> q; // node, depth
    std::unordered_set<NodeId> visited;
    q.push({root, 0});
    visited.insert(root);

    std::size_t marked = root_was_clean ? 1 : 0;
    std::uint32_t max_depth = 0;

    while (!q.empty()) {
        auto [cur, depth] = q.front();
        q.pop();
        max_depth = std::max(max_depth, depth);
        const auto* deps = g.dependents(cur);
        if (!deps)
            continue;
        for (NodeId nxt : *deps) {
            if (visited.contains(nxt))
                continue;
            visited.insert(nxt);
            if (!set.is_dirty(nxt)) {
                set.mark(nxt);
                ++marked;
            } else {
                // Already dirty — still traverse (other paths may reach new nodes).
                // But mark() is idempotent; count only new marks.
            }
            // Ensure dirty even if already marked (propagate_edge for stats).
            set.propagate_edge(cur, nxt);
            // Issue #2063: summary-dirty early-exit. If nxt is already
            // dirty AND its entire dependent cone is already dirty, we
            // can skip the BFS expansion below it (a previously-completed
            // cascade has fully marked its subtree, so re-visiting those
            // nodes produces zero new marks). The skip_count metric makes
            // this observable to the Agent.
            bool skip_subtree = false;
            if (set.is_dirty(nxt) && depth + 1 > 0) {
                const auto* sub_deps = g.dependents(nxt);
                bool all_dirty = sub_deps && !sub_deps->empty();
                if (all_dirty) {
                    for (NodeId sub_nxt : *sub_deps) {
                        if (!set.is_dirty(sub_nxt)) {
                            all_dirty = false;
                            break;
                        }
                    }
                }
                if (all_dirty && sub_deps && !sub_deps->empty()) {
                    skip_subtree = true;
                    // Issue #2063 / #2106: bump file-scope dirty_skip_subtree.
                    // End-of-cascade flush_dirty_skip_subtree_to_metrics()
                    // exchanges into CompilerMetrics::cascade_skip_subtree_total
                    // when the service has registered the metrics sink.
                    dirty_skip_subtree.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (!skip_subtree)
                q.push({nxt, depth + 1});
        }
    }

    dirty_cascade_depth_sum.fetch_add(max_depth, std::memory_order_relaxed);
    dirty_cascade_depth_samples.fetch_add(1, std::memory_order_relaxed);
    dirty_cascade_nodes_marked_total.fetch_add(marked, std::memory_order_relaxed);
    g_dirty_propagation_stats.bfs_nodes_marked += marked;
    // Issue #2106: transfer summary-dirty skips into CompilerMetrics.
    // Exchange zeros dirty_skip_subtree so a subsequent flush (or nested
    // cascade_mark_dirty_many entry) does not re-add the same events.
    (void)flush_dirty_skip_subtree_to_metrics();
    return marked;
}

// Alias: compute transitive dirty closure from root.
inline std::size_t propagate_closure(DirtySet& set, NodeId root, const DepGraph& g) {
    return cascade_mark_dirty(set, root, g);
}

// Multi-root cascade (union of closures).
inline std::size_t cascade_mark_dirty_many(DirtySet& set, std::span<const NodeId> roots,
                                           const DepGraph& g) {
    std::size_t total = 0;
    for (NodeId r : roots)
        total += cascade_mark_dirty(set, r, g);
    return total;
}

// Drain thread-local cascade roots into g_global_dirty (pass_manager hook).
// Issue #2106: each cascade_mark_dirty already flushes skip counts into
// metrics; final flush is a no-op safety net if any residual remains.
inline std::size_t flush_pipeline_cascade_roots() {
    if (!g_pipeline_dep_graph || t_pipeline_cascade_roots.empty()) {
        t_pipeline_cascade_roots.clear();
        (void)flush_dirty_skip_subtree_to_metrics();
        return 0;
    }
    std::size_t n = 0;
    for (NodeId r : t_pipeline_cascade_roots)
        n += cascade_mark_dirty(g_global_dirty, r, *g_pipeline_dep_graph);
    t_pipeline_cascade_roots.clear();
    (void)flush_dirty_skip_subtree_to_metrics();
    return n;
}

// ── IR dirty bridges (#1575 AC3) ───────────────────────────────
// Map a per-block (or per-instruction) dirty column into DirtySet
// at [base, base+n). Inverse pushes DirtySet bits back to the column.
inline void sync_from_ir_dirty(DirtySet& dest, std::span<const std::uint8_t> ir_dirty,
                               NodeId base = 0) {
    dirty_sync_from_ir_total.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < ir_dirty.size(); ++i) {
        if (ir_dirty[i])
            dest.mark(base + static_cast<NodeId>(i));
    }
}

inline void push_to_ir_dirty(const DirtySet& src, std::span<std::uint8_t> ir_dirty,
                             NodeId base = 0) {
    dirty_push_to_ir_total.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < ir_dirty.size(); ++i) {
        if (src.is_dirty(base + static_cast<NodeId>(i)))
            ir_dirty[i] = 1;
    }
}

// Convenience: merge local DirtySet into g_global_dirty.
inline void push_to_global(const DirtySet& src) {
    for (std::size_t i = 0; i < src.bits.size(); ++i) {
        if (src.bits[i])
            g_global_dirty.mark(static_cast<NodeId>(i));
    }
    for (NodeId id : src.sparse)
        g_global_dirty.mark(id);
}

// Convenience: copy g_global_dirty into dest (OR-merge).
inline void pull_from_global(DirtySet& dest) {
    for (std::size_t i = 0; i < g_global_dirty.bits.size(); ++i) {
        if (g_global_dirty.bits[i])
            dest.mark(static_cast<NodeId>(i));
    }
    for (NodeId id : g_global_dirty.sparse)
        dest.mark(id);
}

// Encode (func_idx, block_idx) into a dense NodeId space for bridges.
// Layout: node = (func_idx << 16) | block_idx  (block_idx < 65536).
// High bit clear → block node (Issue #2110 hybrid: distinct from fn nodes).
[[nodiscard]] inline NodeId encode_block_node(std::uint16_t func_idx,
                                              std::uint16_t block_idx) noexcept {
    return (static_cast<NodeId>(func_idx) << 16) | static_cast<NodeId>(block_idx);
}

[[nodiscard]] inline std::pair<std::uint16_t, std::uint16_t> decode_block_node(NodeId id) noexcept {
    return {static_cast<std::uint16_t>(id >> 16), static_cast<std::uint16_t>(id & 0xFFFFu)};
}

// Issue #2110: function-level NodeId for hybrid cascade with string dep_graph_.
// Layout: high bit set, lower 31 bits = dense slot assigned by CompilerService.
// Dirty propagates along edges the same way as block nodes (callee → caller).
inline constexpr NodeId kFnNodeTag = 0x80000000u;

[[nodiscard]] inline NodeId encode_fn_node(std::uint32_t fn_slot) noexcept {
    return kFnNodeTag | (static_cast<NodeId>(fn_slot) & 0x7FFFFFFFu);
}

[[nodiscard]] inline bool is_fn_node(NodeId id) noexcept {
    return (id & kFnNodeTag) != 0;
}

[[nodiscard]] inline std::uint32_t decode_fn_slot(NodeId id) noexcept {
    return static_cast<std::uint32_t>(id & 0x7FFFFFFFu);
}

// Issue #2187: block-level DepGraph edge targets (cross-define unique).
// Distinct from encode_block_node (IR bridge local (fi,bi) without
// caller identity). Layout:
//   bit31 = 0 (not fn node)
//   bit30 = 1 (block-dep tag)
//   bits[29:15] = caller_fn_slot (15 bits)
//   bits[14:8]  = func_idx within define (7 bits)
//   bits[7:0]   = block_idx (8 bits)
// Dirty propagates: encode_fn_node(callee) → encode_block_dep_node(caller,…)
// so cascade from a mutated callee marks only the call-site block.
inline constexpr NodeId kBlockDepTag = 0x40000000u;

[[nodiscard]] inline NodeId encode_block_dep_node(std::uint32_t caller_slot, std::uint16_t func_idx,
                                                  std::uint16_t block_idx) noexcept {
    return kBlockDepTag | ((static_cast<NodeId>(caller_slot & 0x7FFFu)) << 15) |
           ((static_cast<NodeId>(func_idx & 0x7Fu)) << 8) | static_cast<NodeId>(block_idx & 0xFFu);
}

[[nodiscard]] inline bool is_block_dep_node(NodeId id) noexcept {
    return (id & kFnNodeTag) == 0 && (id & kBlockDepTag) != 0;
}

struct BlockDepDecode {
    std::uint32_t caller_slot = 0;
    std::uint16_t func_idx = 0;
    std::uint16_t block_idx = 0;
};

[[nodiscard]] inline BlockDepDecode decode_block_dep_node(NodeId id) noexcept {
    BlockDepDecode d;
    d.caller_slot = static_cast<std::uint32_t>((id >> 15) & 0x7FFFu);
    d.func_idx = static_cast<std::uint16_t>((id >> 8) & 0x7Fu);
    d.block_idx = static_cast<std::uint16_t>(id & 0xFFu);
    return d;
}

// Issue #2191: AST NodeId cone for type partial ↔ IR cascade unify.
// Layout (distinct from fn / block-dep / local block encodings):
//   bit31 = 0, bit30 = 0, bit29 = 1 (kAstDepTag)
//   bits[28:0] = FlatAST NodeId
// Used so type-affected subtrees share a dirty cone with hybrid
// cascade (#2110/#2187) without colliding with encode_block_node.
inline constexpr NodeId kAstDepTag = 0x20000000u;

[[nodiscard]] inline NodeId encode_ast_dep_node(NodeId ast_nid) noexcept {
    return kAstDepTag | (ast_nid & 0x1FFFFFFFu);
}

[[nodiscard]] inline bool is_ast_dep_node(NodeId id) noexcept {
    return (id & kFnNodeTag) == 0 && (id & kBlockDepTag) == 0 && (id & kAstDepTag) != 0;
}

[[nodiscard]] inline NodeId decode_ast_dep_node(NodeId id) noexcept {
    return id & 0x1FFFFFFFu;
}

// Last mirrored type cone (AST NodeIds, not encoded). Thread-local for
// tests / Agent dashboards after infer_flat_partial.
inline thread_local std::vector<NodeId> t_last_type_cone_ast{};

[[nodiscard]] inline const std::vector<NodeId>& last_type_cone_ast() noexcept {
    return t_last_type_cone_ast;
}

// Issue #2556: soft size of the hybrid type∪IR dirty cone for DCE Agents.
// |last type AST cone| + non-AST dirty nodes (fn/block encodings). Zero
// allocation; empty → 0 so DeadCoercionPass takes full-scan or soft-empty path.
[[nodiscard]] inline std::size_t type_ir_union_cone_size() noexcept {
    std::size_t ir_n = 0;
    for (NodeId d : g_global_dirty.dirty_nodes()) {
        if (!is_ast_dep_node(d))
            ++ir_n;
    }
    return t_last_type_cone_ast.size() + ir_n;
}

// Issue #2556: true when type∪IR cone has any mark (partial DCE eligible).
[[nodiscard]] inline bool type_ir_union_cone_nonempty() noexcept {
    if (!t_last_type_cone_ast.empty())
        return true;
    return !g_global_dirty.dirty_nodes().empty();
}

[[nodiscard]] inline double type_ir_cone_union_size_avg() noexcept {
    const auto n = type_ir_cone_union_samples.load(std::memory_order_relaxed);
    if (n == 0)
        return 0.0;
    return static_cast<double>(type_ir_cone_union_size_sum.load(std::memory_order_relaxed)) /
           static_cast<double>(n);
}

// Pull cascade / global-dirty AST dep nodes into out (decoded NodeIds).
// Used to seed infer_flat_partial when IR cascade marked AST-linked dirty.
inline void pull_cascade_ast_dirty_into(std::vector<NodeId>& out) {
    for (NodeId d : g_global_dirty.dirty_nodes()) {
        if (!is_ast_dep_node(d))
            continue;
        out.push_back(decode_ast_dep_node(d));
    }
    // Also scan pending pipeline cascade roots not yet flushed.
    for (NodeId r : t_pipeline_cascade_roots) {
        if (!is_ast_dep_node(r))
            continue;
        out.push_back(decode_ast_dep_node(r));
    }
}

// Issue #2191 / #2516: after infer_flat_partial re-infer (phase 3 of the
// dirty txn), mirror the post-infer type cone into the pipeline DepGraph
// cascade so DirtyAware / partial re-lower see type ∪ IR authority.
// Must run AFTER invalidate_type_dep + re-infer re-record (#2516 AC3);
// empty span → zero cost (AC4).
// Returns number of distinct AST nodes mirrored.
inline std::size_t mirror_type_affected_to_cascade(std::span<const NodeId> affected_ast) {
    t_last_type_cone_ast.clear();
    if (affected_ast.empty()) {
        // Still sample union size so avg tracks quiet windows.
        std::size_t ir_n = 0;
        for (NodeId d : g_global_dirty.dirty_nodes()) {
            if (!is_ast_dep_node(d))
                ++ir_n;
        }
        type_ir_cone_union_size_sum.fetch_add(ir_n, std::memory_order_relaxed);
        type_ir_cone_union_samples.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    std::unordered_set<NodeId> seen;
    seen.reserve(affected_ast.size() * 2);
    t_last_type_cone_ast.reserve(affected_ast.size());
    std::size_t mirrored = 0;
    for (NodeId nid : affected_ast) {
        if (nid == 0)
            continue; // NULL_NODE
        if (!seen.insert(nid).second)
            continue;
        t_last_type_cone_ast.push_back(nid);
        const NodeId dep = encode_ast_dep_node(nid);
        note_pipeline_cascade_root(dep);
        // Immediate mark so reverse pull + DirtySet queries see the cone
        // even before flush_pipeline_cascade_roots (pass_manager entry).
        g_global_dirty.mark(dep);
        ++mirrored;
    }
    if (mirrored)
        type_dirty_cone_mirrored_total.fetch_add(mirrored, std::memory_order_relaxed);

    // Union size = |type AST cone| + |non-AST dirty in global DirtySet|
    // (fn / block-dep / local block encodings from #2110/#2187).
    std::size_t ir_n = 0;
    for (NodeId d : g_global_dirty.dirty_nodes()) {
        if (!is_ast_dep_node(d))
            ++ir_n;
    }
    const std::size_t union_sz = mirrored + ir_n;
    type_ir_cone_union_size_sum.fetch_add(union_sz, std::memory_order_relaxed);
    type_ir_cone_union_samples.fetch_add(1, std::memory_order_relaxed);
    return mirrored;
}

// Sync multi-function block dirty matrix [func][block] into DirtySet.
inline void sync_from_block_dirty_matrix(DirtySet& dest,
                                         const std::vector<std::vector<std::uint8_t>>& per_func) {
    dirty_sync_from_ir_total.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t fi = 0; fi < per_func.size(); ++fi) {
        const auto& fb = per_func[fi];
        for (std::size_t bi = 0; bi < fb.size(); ++bi) {
            if (fb[bi])
                dest.mark(encode_block_node(static_cast<std::uint16_t>(fi),
                                            static_cast<std::uint16_t>(bi)));
        }
    }
}

inline void push_to_block_dirty_matrix(const DirtySet& src,
                                       std::vector<std::vector<std::uint8_t>>& per_func) {
    dirty_push_to_ir_total.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t fi = 0; fi < per_func.size(); ++fi) {
        auto& fb = per_func[fi];
        for (std::size_t bi = 0; bi < fb.size(); ++bi) {
            if (src.is_dirty(encode_block_node(static_cast<std::uint16_t>(fi),
                                               static_cast<std::uint16_t>(bi))))
                fb[bi] = 1;
        }
    }
}


// Issue #2247: pure parity primitives. Both helpers are pure — no
// metrics bump, no side effects beyond the graphs passed in.
//
// graphs_consistent: returns true when every string-graph edge
// (callee → caller via dep_graph_[callee].called_by) has a
// corresponding NodeId edge (encode_fn_node(callee_slot) →
// encode_fn_node(caller_slot)) in node_dep. Compares edge counts
// + per-fn dependent sets.
//
// name_to_slot maps string names → fn slots (CompilerService maintains
// this via dep_name_to_slot_; passed in here as a const ref so we
// don't need a back-pointer into CompilerService).
//
// FunctionDepEntry mirrors CompilerService::DepEntry for pure helpers
// (called_by is the reverse edge used by cascade / parity). Template
// helpers below accept any Entry with a called_by vector so service
// DepEntry and this test-side type both work.
struct FunctionDepEntry {
    std::vector<std::string> calls;
    std::vector<std::string> called_by;
};

template <typename Entry>
[[nodiscard]] inline bool graphs_consistent(
    const std::unordered_map<std::string, Entry, aura::core::TransparentStringHash,
                             std::equal_to<>>& string_dep,
    const DepGraph& node_dep,
    const std::unordered_map<std::string, std::uint32_t, aura::core::TransparentStringHash,
                             std::equal_to<>>& name_to_slot) noexcept {
    // For each string-graph callee→caller edge, check corresponding
    // NodeId edge exists. If any missing, return false.
    for (const auto& [callee, entry] : string_dep) {
        const auto callee_it = name_to_slot.find(callee);
        if (callee_it == name_to_slot.end())
            continue; // never mirrored; not a parity violation
        const auto fn_from = encode_fn_node(callee_it->second);
        for (const auto& caller : entry.called_by) {
            const auto caller_it = name_to_slot.find(caller);
            if (caller_it == name_to_slot.end())
                continue; // never mirrored; not a parity violation
            const auto fn_to = encode_fn_node(caller_it->second);
            const auto* deps = node_dep.dependents(fn_from);
            if (!deps)
                return false;
            bool found = false;
            for (const auto& n : *deps) {
                if (n == fn_to) {
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
    }
    return true;
}

// Rebuild NodeId-graph from string-graph. Clears node_dep.adj and
// re-populates from string-graph via encode_fn_node mapping. Pure
// (no metrics). Caller bumps the dual_dep_graph_parity_fail_total
// metric after this returns (so tests can verify the metric path).
template <typename Entry>
inline void rebuild_node_dep_graph_from_string(
    DepGraph& node_dep,
    const std::unordered_map<std::string, Entry, aura::core::TransparentStringHash,
                             std::equal_to<>>& string_dep,
    const std::unordered_map<std::string, std::uint32_t, aura::core::TransparentStringHash,
                             std::equal_to<>>& name_to_slot) noexcept {
    node_dep.adj.clear();
    for (const auto& [callee, entry] : string_dep) {
        const auto callee_it = name_to_slot.find(callee);
        if (callee_it == name_to_slot.end())
            continue;
        const auto fn_from = encode_fn_node(callee_it->second);
        for (const auto& caller : entry.called_by) {
            const auto caller_it = name_to_slot.find(caller);
            if (caller_it == name_to_slot.end())
                continue;
            const auto fn_to = encode_fn_node(caller_it->second);
            node_dep.add_edge(fn_from, fn_to);
        }
    }
}

// Issue #2247: parity check counter (process-atomic; mirrors the
// per-CompilerMetrics counter in observability_metrics.h). Useful for
// pure unit tests that don't have a CompilerService instance.
inline std::atomic<std::uint64_t>& g_dual_dep_graph_parity_check_total_atomic() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}

inline std::atomic<std::uint64_t>& g_dual_dep_graph_parity_fail_total_atomic() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}

// File-scope Strict toggle (mirrors g_source_to_ir_strict from #2244).
// When set: ensure_dual_dep_graph_parity forces NodeId rebuild on
// mismatch + marks all callers dirty. When 0 (default / unit-test):
// rebuild optional, no hard force.
inline std::atomic<std::uint8_t>& g_dual_dep_graph_strict_atomic() noexcept {
    static std::atomic<std::uint8_t> v{0};
    return v;
}

inline void set_dual_dep_graph_strict(int strict_mode) noexcept {
    g_dual_dep_graph_strict_atomic().store(strict_mode != 0 ? 1 : 0, std::memory_order_relaxed);
}

inline bool dual_dep_graph_strict_enabled() noexcept {
    return g_dual_dep_graph_strict_atomic().load(std::memory_order_relaxed) != 0;
}

} // namespace aura::compiler::dirty

extern "C" std::uint64_t aura_dual_dep_graph_parity_check_v_read() noexcept {
    return aura::compiler::dirty::g_dual_dep_graph_parity_check_total_atomic().load(
        std::memory_order_relaxed);
}

extern "C" std::uint64_t aura_dual_dep_graph_parity_fail_v_read() noexcept {
    return aura::compiler::dirty::g_dual_dep_graph_parity_fail_total_atomic().load(
        std::memory_order_relaxed);
}

extern "C" void aura_set_dual_dep_graph_strict(int strict_mode) noexcept {
    aura::compiler::dirty::set_dual_dep_graph_strict(strict_mode);
}

extern "C" void aura_test_set_dual_dep_graph_strict(int v) noexcept {
    aura::compiler::dirty::set_dual_dep_graph_strict(v);
}
