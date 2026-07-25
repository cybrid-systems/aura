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

#include <atomic>
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
#include "hash_meta.h"                     // FNV constants (#901)
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>

export module aura.compiler.ir_cache_pure;

import aura.core.ast;
import aura.compiler.ir;

export namespace aura::compiler {

// ── CacheEntryVersionStamp (#2033) ────────────────────────
// Monotonic stamp binding mutation / bridge / defuse domains
// so should_relower cannot silently serve IR after epoch bumps
// when dirty/source_hash look clean (high-freq AI self-mod).
struct CacheEntryVersionStamp {
    std::uint64_t mutation_count = 0; // mutation_epoch at lower/store
    std::uint64_t bridge_epoch = 0;
    std::uint64_t defuse_version = 0;
    [[nodiscard]] bool is_stamped() const noexcept {
        return mutation_count != 0 || bridge_epoch != 0 || defuse_version != 0;
    }
};

// Reason bitflags for should_relower observability (optional out-param).
inline constexpr std::uint32_t kRelowerDirty = 1u << 0;
inline constexpr std::uint32_t kRelowerSourceHash = 1u << 1;
inline constexpr std::uint32_t kRelowerMutationDrift = 1u << 2;
inline constexpr std::uint32_t kRelowerBridgeEpoch = 1u << 3;
inline constexpr std::uint32_t kRelowerDefuseVersion = 1u << 4;

// ── should_relower ────────────────────────────────────────
// Issue #126 / #2033: pure decision function extracted from
// CompilerService::lookup_define_v2.
//
// Returns true iff the cached IR entry needs to be re-lowered:
//   - dirty flag
//   - source-hash mismatch
//   - mutation-count / mutation_epoch drift
//   - bridge_epoch mismatch (when stamp was written)
//   - defuse_version drift (when both non-zero)
//
// Pure: same inputs → same output. reasons_out optional bitmask.
[[nodiscard]] inline bool should_relower(std::size_t source_hash, std::size_t cached_source_hash,
                                         bool dirty, const CacheEntryVersionStamp& stamp,
                                         std::uint64_t current_mutation_count,
                                         std::uint64_t current_bridge_epoch,
                                         std::uint64_t current_defuse_version = 0,
                                         std::uint32_t* reasons_out = nullptr) noexcept {
    std::uint32_t reasons = 0;
    if (dirty)
        reasons |= kRelowerDirty;
    if (source_hash != cached_source_hash)
        reasons |= kRelowerSourceHash;
    if (stamp.mutation_count < current_mutation_count)
        reasons |= kRelowerMutationDrift;
    // Issue #2033: bridge_epoch check closes silent-stale when dirty cleared
    // inconsistently but bridge views already expired.
    if (stamp.bridge_epoch != 0 && stamp.bridge_epoch != current_bridge_epoch)
        reasons |= kRelowerBridgeEpoch;
    if (stamp.defuse_version != 0 && current_defuse_version != 0 &&
        stamp.defuse_version != current_defuse_version)
        reasons |= kRelowerDefuseVersion;
    if (reasons_out)
        *reasons_out = reasons;
    return reasons != 0;
}

// Backward-compatible overload (pre-#2033 call sites).
// Does not apply bridge/defuse checks (zeros).
[[nodiscard]] inline bool should_relower(std::size_t source_hash, std::size_t cached_source_hash,
                                         bool dirty, std::uint64_t cached_mutation_count,
                                         std::uint64_t current_mutation_count) noexcept {
    CacheEntryVersionStamp stamp;
    stamp.mutation_count = cached_mutation_count;
    return should_relower(source_hash, cached_source_hash, dirty, stamp, current_mutation_count,
                          /*current_bridge_epoch=*/0, /*current_defuse_version=*/0, nullptr);
}

// ── compute_impact_scope ──────────────────────────────────
// Issue #460 / #2031: pure walker that computes the per-block /
// per-instruction impact of a mutation rooted at `root`.
// Returns affected (function, block) pairs and precise
// (function, block, instr) triples when the source map has
// instruction indices (from IRInstruction::source_ast_node_id).
//
// Based on:
//   1. The AST subtree rooted at `root` (the mutated node)
//   2. The `source_to_ir_map` (NodeId → function/block[/instr])
//   3. The `ir_cache_index` (function name → index; reserved)
//
// The function is pure: same inputs → same output.

// Issue #2031: location of one AST node in lowered IR.
// instr_index == UINT32_MAX means block-level only (no precise instr).
struct SourceIrLoc {
    std::size_t function_index = 0;
    std::uint32_t block_index = 0;
    std::uint32_t instr_index = UINT32_MAX;
    [[nodiscard]] bool has_instr() const noexcept { return instr_index != UINT32_MAX; }
};

struct ImpactScope {
    struct BlockRef {
        std::size_t function_index; // index in ir_cache_index / irs[]
        std::uint32_t block_index;
    };
    // Issue #2031: precise instruction impact (subset of blocks).
    struct InstrRef {
        std::size_t function_index = 0;
        std::uint32_t block_index = 0;
        std::uint32_t instr_index = 0; // in-block index
    };
    std::vector<BlockRef> affected_blocks;
    std::vector<InstrRef> affected_instrs; // #2031
    // Issue #2109: alias name used in pipeline review / ACs.
    // Same storage as affected_instrs (reference for readability).
    [[nodiscard]] std::vector<InstrRef>& affected_insts() noexcept { return affected_instrs; }
    [[nodiscard]] const std::vector<InstrRef>& affected_insts() const noexcept {
        return affected_instrs;
    }
    // Number of AST nodes walked (for observability).
    std::size_t ast_nodes_visited = 0;
    // Issue #2031: AST nodes in subtree with no source_to_ir mapping.
    std::size_t unmapped_ast_nodes = 0;
    // Issue #2031: how many mapped locs had a precise instr index.
    std::size_t instr_level_hits = 0;
};

// Map type used by compute_impact_scope (block+optional instr).
using SourceToIrMap = std::unordered_map<aura::ast::NodeId, SourceIrLoc>;

// Backward-compat alias: block-only maps (func, block) without instr.
using SourceToIrBlockMap =
    std::unordered_map<aura::ast::NodeId, std::pair<std::size_t, std::uint32_t>>;

ImpactScope compute_impact_scope(
    const aura::ast::FlatAST& flat, aura::ast::NodeId root, const SourceToIrMap& source_to_ir_map,
    const std::unordered_map<std::string, std::size_t, aura::core::TransparentStringHash,
                             std::equal_to<>>& ir_cache_index) {
    ImpactScope result;
    if (root == aura::ast::NULL_NODE || root >= flat.size()) {
        return result;
    }
    // Walk the AST subtree; collect affected blocks + instructions.
    // Block key: (func << 32) | block. Instr key: (func << 40) | (block << 20) | instr.
    std::unordered_set<std::uint64_t> seen_blocks;
    std::unordered_set<std::uint64_t> seen_instrs;
    auto walk = [&](auto self, aura::ast::NodeId id) -> void {
        if (id == aura::ast::NULL_NODE || id >= flat.size())
            return;
        result.ast_nodes_visited++;
        auto it = source_to_ir_map.find(id);
        if (it != source_to_ir_map.end()) {
            const auto& loc = it->second;
            const auto bkey = (static_cast<std::uint64_t>(loc.function_index) << 32) |
                              static_cast<std::uint64_t>(loc.block_index);
            if (seen_blocks.insert(bkey).second) {
                result.affected_blocks.push_back({loc.function_index, loc.block_index});
            }
            if (loc.has_instr()) {
                const auto ikey = (static_cast<std::uint64_t>(loc.function_index) << 40) |
                                  (static_cast<std::uint64_t>(loc.block_index) << 20) |
                                  static_cast<std::uint64_t>(loc.instr_index);
                if (seen_instrs.insert(ikey).second) {
                    result.affected_instrs.push_back(
                        {loc.function_index, loc.block_index, loc.instr_index});
                    ++result.instr_level_hits;
                }
            }
        } else {
            ++result.unmapped_ast_nodes;
        }
        auto node = flat.get(id);
        for (std::size_t ci = 0; ci < node.children.size(); ++ci) {
            self(self, node.child(ci));
        }
    };
    walk(walk, root);
    (void)ir_cache_index; // reserved for cross-function cascade
    return result;
}

// Issue #2031: overload accepting legacy block-only maps (no instr index).
inline ImpactScope compute_impact_scope(
    const aura::ast::FlatAST& flat, aura::ast::NodeId root,
    const SourceToIrBlockMap& source_to_ir_blocks,
    const std::unordered_map<std::string, std::size_t, aura::core::TransparentStringHash,
                             std::equal_to<>>& ir_cache_index) {
    SourceToIrMap rich;
    rich.reserve(source_to_ir_blocks.size());
    for (const auto& [nid, fb] : source_to_ir_blocks) {
        SourceIrLoc loc;
        loc.function_index = fb.first;
        loc.block_index = fb.second;
        loc.instr_index = UINT32_MAX;
        rich.emplace(nid, loc);
    }
    return compute_impact_scope(flat, root, rich, ir_cache_index);
}

// ── source_to_ir_map rebuild / consistency (Issue #2045) ───
// Impact-scope selective invalidation depends on an accurate
// NodeId → (func, block[, instr]) reverse index. After every
// partial or full re-lower the map must be rebuilt or patched
// against the new IR layout so the next mutate cannot under-
// invalidate (silent stale blocks).
//
// Helpers are pure: they only read `irs` / write `out` map.

// Populate (append) reverse index from lowered IR stamps.
// Prefer precise instr when present; keep first precise hit
// for a given NodeId (matches service populate_source_to_ir_from_irs).
inline void populate_source_to_ir_map_from_irs(const std::vector<aura::ir::IRFunction>& irs,
                                               SourceToIrMap& out) {
    for (std::size_t fi = 0; fi < irs.size(); ++fi) {
        const auto& fn = irs[fi];
        for (std::uint32_t bi = 0; bi < static_cast<std::uint32_t>(fn.blocks.size()); ++bi) {
            const auto& blk = fn.blocks[bi];
            for (std::uint32_t ii = 0; ii < static_cast<std::uint32_t>(blk.instructions.size());
                 ++ii) {
                const auto sn = blk.instructions[ii].source_ast_node_id;
                if (sn == 0)
                    continue;
                const auto nid = static_cast<aura::ast::NodeId>(sn);
                SourceIrLoc loc{fi, bi, ii};
                auto it = out.find(nid);
                if (it == out.end() || !it->second.has_instr())
                    out[nid] = loc;
            }
        }
    }
}

// Full rebuild: clear + populate from current irs layout.
inline void rebuild_source_to_ir_map_from_irs(const std::vector<aura::ir::IRFunction>& irs,
                                              SourceToIrMap& out) {
    out.clear();
    if (!irs.empty())
        out.reserve(irs.size() * 8);
    populate_source_to_ir_map_from_irs(irs, out);
}

// Incremental patch after per-function re-lower: drop all
// entries pointing at `func_idx`, then re-stamp from that
// function's new blocks only.
inline void patch_source_to_ir_map_for_function(const aura::ir::IRFunction& fn,
                                                std::size_t func_idx, SourceToIrMap& out) {
    for (auto it = out.begin(); it != out.end();) {
        if (it->second.function_index == func_idx)
            it = out.erase(it);
        else
            ++it;
    }
    for (std::uint32_t bi = 0; bi < static_cast<std::uint32_t>(fn.blocks.size()); ++bi) {
        const auto& blk = fn.blocks[bi];
        for (std::uint32_t ii = 0; ii < static_cast<std::uint32_t>(blk.instructions.size()); ++ii) {
            const auto sn = blk.instructions[ii].source_ast_node_id;
            if (sn == 0)
                continue;
            const auto nid = static_cast<aura::ast::NodeId>(sn);
            SourceIrLoc loc{func_idx, bi, ii};
            auto it = out.find(nid);
            if (it == out.end() || !it->second.has_instr())
                out[nid] = loc;
        }
    }
}

// Count map entries whose (func, block[, instr]) is not a live
// location in `irs`, or whose reverse stamp no longer matches
// the mapped NodeId (stale after re-lower).
[[nodiscard]] inline std::size_t
count_source_to_ir_map_inconsistencies(const std::vector<aura::ir::IRFunction>& irs,
                                       const SourceToIrMap& map) noexcept {
    std::size_t bad = 0;
    for (const auto& [nid, loc] : map) {
        if (loc.function_index >= irs.size()) {
            ++bad;
            continue;
        }
        const auto& fn = irs[loc.function_index];
        if (loc.block_index >= fn.blocks.size()) {
            ++bad;
            continue;
        }
        if (!loc.has_instr())
            continue;
        const auto& blk = fn.blocks[loc.block_index];
        if (loc.instr_index >= blk.instructions.size()) {
            ++bad;
            continue;
        }
        const auto sn = blk.instructions[loc.instr_index].source_ast_node_id;
        // Reverse stamp mismatch = stale map entry (under-invalidation risk).
        if (sn != 0 && static_cast<aura::ast::NodeId>(sn) != nid)
            ++bad;
    }
    return bad;
}

// Cheap consistency predicate for debug / fuzz / tests.
[[nodiscard]] inline bool
source_to_ir_map_is_consistent(const std::vector<aura::ir::IRFunction>& irs,
                               const SourceToIrMap& map) noexcept {
    return count_source_to_ir_map_inconsistencies(irs, map) == 0;
}

// Debug/fuzz assert: returns true when consistent. In debug builds
// with AURA_ASSERT_SOURCE_TO_IR (or !NDEBUG), fires assert on failure.
// Always safe to call in release (returns false on inconsistency).
[[nodiscard]] inline bool
assert_source_to_ir_map_consistent(const std::vector<aura::ir::IRFunction>& irs,
                                   const SourceToIrMap& map) noexcept {
    const bool ok = source_to_ir_map_is_consistent(irs, map);
#if !defined(NDEBUG) || defined(AURA_ASSERT_SOURCE_TO_IR)
    // Soft assert: empty map with empty irs is fine; only fire when
    // map claims mappings that don't resolve to live IR.
    if (!ok && !map.empty()) {
        // Intentionally not aborting production fuzz that injects
        // intentional desync — callers treat return value as the
        // contract. Keep a side-channel assert for unit tests that
        // define AURA_ASSERT_SOURCE_TO_IR_HARD.
#if defined(AURA_ASSERT_SOURCE_TO_IR_HARD)
        assert(ok && "source_to_ir_map inconsistent with IR layout (#2045)");
#endif
        (void)ok;
    }
#else
    (void)ok;
#endif
    return ok;
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
    std::size_t h = ::aura::compiler::stats::kFnvOffsetBasis;
    for (unsigned char c : s) {
        h ^= static_cast<std::size_t>(c);
        h *= ::aura::compiler::stats::kFnvPrime;
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

// Issue #426 / #2032: configurable partial-vs-full re-lower threshold.
// Default 8 (historical): 0 → skip, 1..(T-1) → partial, ≥T → full.
// Agents / tests may raise (bulk set-code) or lower (tiny AI edits).
inline constexpr std::size_t kDefaultPartialRelowerThreshold = 8;

inline std::atomic<std::size_t>& partial_relower_threshold_atomic() noexcept {
    static std::atomic<std::size_t> thr{kDefaultPartialRelowerThreshold};
    return thr;
}

[[nodiscard]] inline std::size_t get_partial_relower_threshold() noexcept {
    return partial_relower_threshold_atomic().load(std::memory_order_relaxed);
}

// Clamp: 0 → 1 (never all-skip); very large values still mean "prefer partial".
inline void set_partial_relower_threshold(std::size_t t) noexcept {
    if (t == 0)
        t = 1;
    partial_relower_threshold_atomic().store(t, std::memory_order_relaxed);
}

inline void reset_partial_relower_threshold_for_test() noexcept {
    set_partial_relower_threshold(kDefaultPartialRelowerThreshold);
}

// Issue #426 / #2032: estimate the re-lower cost of a dirty
// function. Returns the number of blocks that need
// re-lowering. If the function is fully clean (no dirty
// bits), returns 0. If dirty_count >= threshold, returns
// std::size_t(-1) as a sentinel meaning "re-lower the whole
// function".
//
// Heuristic (threshold T, default 8):
//   - 0 dirty blocks → 0 (skip)
//   - 1..(T-1) dirty blocks → exact count (incremental)
//   - T+ dirty blocks → std::size_t(-1) (full re-lower)
[[nodiscard]] inline std::size_t estimate_relower_blocks(std::size_t dirty_count) noexcept {
    if (dirty_count == 0)
        return 0;
    if (dirty_count >= get_partial_relower_threshold())
        return static_cast<std::size_t>(-1);
    return dirty_count;
}

// Pure decision with explicit threshold (unit tests / offline A/B).
[[nodiscard]] constexpr std::size_t estimate_relower_blocks(std::size_t dirty_count,
                                                            std::size_t threshold) noexcept {
    if (dirty_count == 0)
        return 0;
    if (threshold == 0)
        threshold = 1;
    if (dirty_count >= threshold)
        return static_cast<std::size_t>(-1);
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
summarize_block_dirty(const std::vector<std::vector<std::uint8_t>>& block_dirty_per_func) noexcept {
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

// Issue #718 / #2032: partial-relower decision helper. Returns true
// when the dirty block mask should trigger a partial re-lower
// (1..(T-1) dirty blocks) instead of a full re-lower (≥T) or
// no-op (0). Threshold T is process-wide (default 8), overridable
// via set_partial_relower_threshold / HotUpdateRegistry.
//
// Used by:
//   - service.ixx::invalidate_function / cache_define paths
//   - lowering_impl.cpp::lower_to_ir_with_cache
//   - pass_manager.ixx::run_incremental_pipeline
//
// Consistent with estimate_relower_blocks bucketing.
[[nodiscard]] inline bool should_partial_relower(std::size_t dirty_count) noexcept {
    if (dirty_count == 0)
        return false;
    if (dirty_count >= get_partial_relower_threshold())
        return false;
    return true;
}

// Pure decision with explicit threshold (tests / offline A/B).
[[nodiscard]] constexpr bool should_partial_relower(std::size_t dirty_count,
                                                    std::size_t threshold) noexcept {
    if (dirty_count == 0)
        return false;
    if (threshold == 0)
        threshold = 1;
    if (dirty_count >= threshold)
        return false;
    return true;
}

} // namespace aura::compiler
