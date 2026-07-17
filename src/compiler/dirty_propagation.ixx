// dirty_propagation.ixx — Issue #1206 Phase 1 scaffold + Issue #1575 Phase 2:
// automatic BFS cascade, IR dirty bridges, DirtyAwarePass integration hooks.

module;

export module aura.compiler.dirty_propagation;

import std;

export namespace aura::compiler::dirty {

inline constexpr int kDirtyPropagationPhase = 2; // #1575: cascade + bridges

using NodeId = std::uint32_t;

// ── Metrics (#1575) ────────────────────────────────────────────
// Module-level atomics so Agent / tests can observe cascade health
// without going through CompilerService.
inline std::atomic<std::uint64_t> dirty_propagation_bfs_hits{0};
inline std::atomic<std::uint64_t> manual_propagate_deprecated_count{0};
// Running sum of BFS depths + sample count → dirty_cascade_depth_avg.
inline std::atomic<std::uint64_t> dirty_cascade_depth_sum{0};
inline std::atomic<std::uint64_t> dirty_cascade_depth_samples{0};
inline std::atomic<std::uint64_t> dirty_cascade_nodes_marked_total{0};
inline std::atomic<std::uint64_t> dirty_sync_from_ir_total{0};
inline std::atomic<std::uint64_t> dirty_push_to_ir_total{0};

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

// ── DirtySet: bitset-backed dirty flags ────────────────────────
struct DirtySet {
    std::vector<bool> bits;

    void ensure(std::size_t n) {
        if (bits.size() < n)
            bits.resize(n, false);
    }

    void mark(NodeId id) {
        ensure(static_cast<std::size_t>(id) + 1);
        bits[id] = true;
        ++g_dirty_propagation_stats.marks;
    }

    // Issue #1575: non-stats peek for bridges / cascade internals.
    [[nodiscard]] bool is_dirty(NodeId id) const noexcept { return id < bits.size() && bits[id]; }

    // Issue #1206 pairwise API — kept for backward compat.
    // Issue #1575: counts as deprecated manual use; prefer cascade_mark_dirty.
    [[deprecated("Issue #1575: prefer cascade_mark_dirty / propagate_closure with DepGraph")]]
    void propagate(NodeId from, NodeId to) {
        manual_propagate_deprecated_count.fetch_add(1, std::memory_order_relaxed);
        ensure(static_cast<std::size_t>(std::max(from, to)) + 1);
        if (bits[from]) {
            bits[to] = true;
            ++g_dirty_propagation_stats.propagations;
        }
    }

    // Non-deprecated internal pairwise used by cascade (does not bump
    // manual_propagate_deprecated_count).
    void propagate_edge(NodeId from, NodeId to) {
        ensure(static_cast<std::size_t>(std::max(from, to)) + 1);
        if (bits[from]) {
            bits[to] = true;
            ++g_dirty_propagation_stats.propagations;
        }
    }

    [[nodiscard]] bool query(NodeId id) {
        ++g_dirty_propagation_stats.queries;
        if (id >= bits.size() || !bits[id]) {
            ++g_dirty_propagation_stats.clean_hits;
            return false;
        }
        return true;
    }

    void clear() { bits.assign(bits.size(), false); }

    [[nodiscard]] std::size_t dirty_count() const noexcept {
        std::size_t n = 0;
        for (std::size_t i = 0; i < bits.size(); ++i)
            if (bits[i])
                ++n;
        return n;
    }

    // Collect all currently-dirty node ids (for tests / dashboards).
    [[nodiscard]] std::vector<NodeId> dirty_nodes() const {
        std::vector<NodeId> out;
        for (std::size_t i = 0; i < bits.size(); ++i)
            if (bits[i])
                out.push_back(static_cast<NodeId>(i));
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
            q.push({nxt, depth + 1});
        }
    }

    dirty_cascade_depth_sum.fetch_add(max_depth, std::memory_order_relaxed);
    dirty_cascade_depth_samples.fetch_add(1, std::memory_order_relaxed);
    dirty_cascade_nodes_marked_total.fetch_add(marked, std::memory_order_relaxed);
    g_dirty_propagation_stats.bfs_nodes_marked += marked;
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
inline std::size_t flush_pipeline_cascade_roots() {
    if (!g_pipeline_dep_graph || t_pipeline_cascade_roots.empty()) {
        t_pipeline_cascade_roots.clear();
        return 0;
    }
    std::size_t n = 0;
    for (NodeId r : t_pipeline_cascade_roots)
        n += cascade_mark_dirty(g_global_dirty, r, *g_pipeline_dep_graph);
    t_pipeline_cascade_roots.clear();
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
}

// Convenience: copy g_global_dirty into dest (OR-merge).
inline void pull_from_global(DirtySet& dest) {
    for (std::size_t i = 0; i < g_global_dirty.bits.size(); ++i) {
        if (g_global_dirty.bits[i])
            dest.mark(static_cast<NodeId>(i));
    }
}

// Encode (func_idx, block_idx) into a dense NodeId space for bridges.
// Layout: node = (func_idx << 16) | block_idx  (block_idx < 65536).
[[nodiscard]] inline NodeId encode_block_node(std::uint16_t func_idx,
                                              std::uint16_t block_idx) noexcept {
    return (static_cast<NodeId>(func_idx) << 16) | static_cast<NodeId>(block_idx);
}

[[nodiscard]] inline std::pair<std::uint16_t, std::uint16_t> decode_block_node(NodeId id) noexcept {
    return {static_cast<std::uint16_t>(id >> 16), static_cast<std::uint16_t>(id & 0xFFFFu)};
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

} // namespace aura::compiler::dirty
