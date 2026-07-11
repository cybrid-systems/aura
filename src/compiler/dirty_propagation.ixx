// dirty_propagation.ixx — Issue #1206 Phase 1: unified mark/propagate/query API scaffold.

module;

export module aura.compiler.dirty_propagation;

import std;

export namespace aura::compiler::dirty {

inline constexpr int kDirtyPropagationPhase = 1;

using NodeId = std::uint32_t;

struct DirtyPropagationStats {
    std::uint64_t marks = 0;
    std::uint64_t propagations = 0;
    std::uint64_t queries = 0;
    std::uint64_t clean_hits = 0;
};

inline DirtyPropagationStats g_dirty_propagation_stats{};

// Phase 1 bitset-backed dirty set (single-process). Full IR/FlatAST peel follows.
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

    void propagate(NodeId from, NodeId to) {
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
};

inline DirtySet g_global_dirty{};

} // namespace aura::compiler::dirty
