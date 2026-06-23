// ir_cache_pure.ixx — Pure free functions extracted from
// CompilerService and Evaluator (Issue #126).
//
// These functions take all state as parameters and return
// values without mutating global / member state. They can be
// unit-tested in isolation, composed freely, and used from
// future parallel lowering pipelines without locks.
//
// The "pure" guarantee:
//   - No `this` access (no member mutation)
//   - No I/O (no logging, no file writes)
//   - No global state lookup
//   - No mutation of any input argument (FlatAST, StringPool
//     are taken as non-const because macro expansion in
//     `try_extract_define` is read-only, but the helper
//     itself never mutates them)
//
// What is NOT pure here:
//   - `try_extract_define` calls `pool.resolve(sym_id)`,
//     which is read-only on the pool, so it is pure
//   - `compute_dependencies` walks the AST read-only
//   - `should_relower` is a pure decision function

module;

#include <cstddef>
#include <cstdint>
#include <bit>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

export module aura.compiler.ir_cache_pure;

import aura.core.ast;

export namespace aura::compiler {

// ── should_relower ────────────────────────────────────────
// Issue #126: pure decision function extracted from
// CompilerService::lookup_define_v2 (which combined this
// decision with cache lookups).
//
// Returns true iff the cached IR entry needs to be
// re-lowered. The decision combines:
//   - dirty flag (set by mutate:* / set-code on the entry)
//   - source-hash mismatch (the define body was re-defined)
//   - mutation-count drift (the global mutation counter
//     advanced beyond the snapshot at lower time)
//
// All inputs are values (no reference to the cache entry or
// the global mutation counter), so the function is fully
// pure: same inputs → same output, always.
bool should_relower(std::size_t source_hash, std::size_t cached_source_hash, bool dirty,
                    std::uint64_t cached_mutation_count,
                    std::uint64_t current_mutation_count) noexcept {
    if (dirty)
        return true;
    if (source_hash != cached_source_hash)
        return true;
    if (cached_mutation_count < current_mutation_count)
        return true;
    return false;
}

// ── compute_impact_scope ──────────────────────────────────
// Issue #460: pure walker that computes the per-block /
// per-instruction impact of a mutation rooted at `root`.
// Returns the list of (function-index, block-index) pairs
// that are affected by the mutation. The decision is
// based on:
//   1. The AST subtree rooted at `root` (the mutated node)
//   2. The `source_to_ir_map` (maps each source AST NodeId
//      to its corresponding IR function/block/instruction)
//   3. The `ir_cache_index` (maps each function name to
//      its index in the IR module)
//
// For the P0 ship we return the affected blocks only
// (function-block pairs). Per-instruction impact is a
// follow-up (#460 follow-up 1).
//
// The function is pure: same inputs → same output.
struct ImpactScope {
    struct BlockRef {
        std::size_t function_index; // index in ir_cache_index
        std::uint32_t block_index;
    };
    std::vector<BlockRef> affected_blocks;
    // Number of AST nodes walked (for observability).
    std::size_t ast_nodes_visited = 0;
};
ImpactScope compute_impact_scope(
    const aura::ast::FlatAST& flat, aura::ast::NodeId root,
    const std::unordered_map<aura::ast::NodeId,
                             std::pair<std::size_t, std::uint32_t>>& source_to_ir_map,
    const std::unordered_map<std::string, std::size_t>& ir_cache_index) {
    ImpactScope result;
    if (root == aura::ast::NULL_NODE || root >= flat.size()) {
        return result;
    }
    // Walk the AST subtree and collect affected blocks
    // (function_index, block_index) pairs. Dedupe via a set.
    std::unordered_set<std::uint64_t> seen;
    auto walk = [&](auto self, aura::ast::NodeId id) -> void {
        if (id == aura::ast::NULL_NODE || id >= flat.size()) return;
        result.ast_nodes_visited++;
        auto it = source_to_ir_map.find(id);
        if (it != source_to_ir_map.end()) {
            auto key = (static_cast<std::uint64_t>(it->second.first) << 32) |
                       static_cast<std::uint64_t>(it->second.second);
            if (seen.insert(key).second) {
                result.affected_blocks.push_back({it->second.first, it->second.second});
            }
        }
        auto node = flat.get(id);
        for (std::size_t ci = 0; ci < node.children.size(); ++ci) {
            self(self, node.child(ci));
        }
    };
    walk(walk, root);
    (void)ir_cache_index; // used by follow-up for cross-function cascade
    return result;
}

// ── compute_dependencies ──────────────────────────────────
// Issue #126: pure walker extracted from the local
// DepWalker struct inside CompilerService::record_define
// (which depended on this->ir_cache_ and this->dep_graph_).
//
// Walks the FlatAST rooted at `root` and returns the list
// of names that are both (a) referenced via Variable nodes
// and (b) present in `available_defines` (typically the
// keys of the ir_cache_ map).
//
// The output is in first-encounter order, deduplicated. The
// function never mutates the input FlatAST or StringPool.
std::vector<std::string>
compute_dependencies(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                     aura::ast::NodeId root,
                     const std::unordered_set<std::string>& available_defines) {
    std::vector<std::string> deps;
    std::unordered_set<std::string> seen;

    // Recursive lambda walker. The walk is bounded by
    // the AST size; we never recurse into NULL_NODE or
    // out-of-range ids.
    auto walk = [&](auto self, aura::ast::NodeId id) -> void {
        if (id == aura::ast::NULL_NODE || id >= flat.size())
            return;
        auto nv = flat.get(id);
        if (nv.tag == aura::ast::NodeTag::Variable) {
            std::string_view name = pool.resolve(nv.sym_id);
            if (!name.empty()) {
                std::string name_str(name);
                if (available_defines.count(name_str) && !seen.count(name_str)) {
                    seen.insert(name_str);
                    deps.push_back(name_str);
                }
            }
        }
        for (auto c : nv.children) {
            self(self, c);
        }
    };

    walk(walk, root);
    return deps;
}

// ── try_extract_define ────────────────────────────────────
// Issue #126: pure AST pattern match extracted from the
// private static CompilerService::try_extract_define
// method. The function is now a free function and can be
// called from any context (e.g., from lower_to_ir's static
// helpers, from the REPL frontend, or from test harnesses).
//
// Returns {name, body_node_id} if the root is a Define
// node, otherwise nullopt. No side effects.
std::optional<std::pair<std::string, aura::ast::NodeId>>
try_extract_define(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                   aura::ast::NodeId root) {
    if (root == aura::ast::NULL_NODE)
        return std::nullopt;
    if (root >= flat.size())
        return std::nullopt;
    auto v = flat.get(root);
    if (v.tag == aura::ast::NodeTag::Define) {
        std::string_view name = pool.resolve(v.sym_id);
        if (name.empty())
            return std::nullopt;
        aura::ast::NodeId body = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
        return std::make_pair(std::string(name), body);
    }
    return std::nullopt;
}

// ── FNV-1a 64-bit hash ────────────────────────────────────
// Issue #126: extracted as a pure function so the source
// hash logic can be unit-tested in isolation. Same input
// bytes → same hash, no global state.
std::size_t fnv1a_64(std::string_view s) noexcept {
    std::size_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= static_cast<std::size_t>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

// ── Issue #426: count_dirty_blocks + relower_affected_blocks
// Pure functions for fine-grained re-lower decision + estimate.
// Both take the per-function block_dirty_ bitmask (a vector
// of uint8_t, 1 bit per block) and return a count.
// The "pure" guarantee: same input → same output, no
// global state, no I/O, no this.

// Count the number of set bits in a block_dirty_ mask.
// P0: popcount via std::popcount (C++20).
[[nodiscard]] inline std::size_t
count_dirty_blocks(const std::vector<std::uint8_t>& block_dirty) noexcept {
    std::size_t n = 0;
    for (auto byte : block_dirty) {
        n += static_cast<std::size_t>(std::popcount(byte));
    }
    return n;
}

// Issue #426: estimate the re-lower cost of a dirty
// function. Returns the number of blocks that need
// re-lowering. If the function is fully clean (no dirty
// bits), returns 0. If the function is "fully dirty"
// (all blocks are dirty), returns std::size_t(-1) as a
// sentinel meaning "re-lower the whole function".
//
// Heuristic:
//   - 0 dirty blocks → 0 (skip)
//   - 1..7 dirty blocks → exact count (incremental)
//   - 8+ dirty blocks → std::size_t(-1) (full re-lower)
//
// P0: this is a decision function, not an actual
// re-lower. The follow-up wires the call to the lowering
// pipeline + JIT incremental update.
[[nodiscard]] constexpr std::size_t
estimate_relower_blocks(std::size_t dirty_count) noexcept {
    if (dirty_count == 0) return 0;
    if (dirty_count >= 8) return static_cast<std::size_t>(-1);
    return dirty_count;
}

// Issue #426: aggregate stats over many functions'
// block_dirty_ masks. Returns the total dirty count
// across all functions (for the dirty_rate observability
// primitive) + the number of functions that have at
// least one dirty block (for the "incremental candidates"
// count).
struct BlockDirtySummary {
    std::size_t total_dirty_blocks = 0;
    std::size_t functions_with_dirty = 0;
    std::size_t functions_total = 0;
    // Number of functions that would be incremental
    // re-lower candidates (1..7 dirty blocks).
    std::size_t incremental_candidates = 0;
    // Number of functions that would be full re-lower
    // candidates (8+ dirty blocks).
    std::size_t full_relower_candidates = 0;
};

[[nodiscard]] inline BlockDirtySummary
summarize_block_dirty(
    const std::vector<std::vector<std::uint8_t>>&
        block_dirty_per_func) noexcept {
    BlockDirtySummary s;
    s.functions_total = block_dirty_per_func.size();
    for (const auto& mask : block_dirty_per_func) {
        const auto n = count_dirty_blocks(mask);
        s.total_dirty_blocks += n;
        if (n > 0) {
            ++s.functions_with_dirty;
            const auto est = estimate_relower_blocks(n);
            if (est == static_cast<std::size_t>(-1))
                ++s.full_relower_candidates;
            else
                ++s.incremental_candidates;
        }
    }
    return s;
}

} // namespace aura::compiler
