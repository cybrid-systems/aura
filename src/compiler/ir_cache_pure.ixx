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
#include <cassert>
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
import aura.compiler.dirty_propagation; // DepGraph (Issue #2179)

// Issue #2190: StormLevel facade C ABI (defined in hot_update_registry /
// weak stub). Must live outside the global module fragment (GMF may only
// contain preprocessor inclusions). Global bit forces full relower;
// Shape-only does not.
extern "C" std::uint8_t aura_hot_update_current_storm_level(void);

export namespace aura::compiler {

// ── CacheEntryVersionStamp (#2033 / #2183) ─────────────────
// Monotonic stamp binding mutation / bridge / defuse domains
// so should_relower cannot silently serve IR after epoch bumps
// when dirty/source_hash look clean (high-freq AI self-mod).
//
// Issue #2183 unified restamp contract:
//   Every successful store_define_v2 / partial peel / full relower /
//   cascade store / AOT reemit success MUST call restamp_cache_entry
//   (or CompilerService::restamp_cache_entry_live_) with live atomics.
//   lookup_define_v2 must never serve IR when should_relower reports
//   stamp-domain drift (mutation / bridge / defuse / soa gen).
//   AOT table_generation is joint with bridge via #2046; restamp after
//   reemit keeps IR-cache stamp domains aligned with the joint epoch.
struct CacheEntryVersionStamp {
    std::uint64_t mutation_count = 0; // mutation_epoch at lower/store
    std::uint64_t bridge_epoch = 0;
    std::uint64_t defuse_version = 0;
    // Issue #2111: SoA generation fence at lower/store time.
    std::uint64_t soa_generation = 0;
    [[nodiscard]] bool is_stamped() const noexcept {
        return mutation_count != 0 || bridge_epoch != 0 || defuse_version != 0 ||
               soa_generation != 0;
    }
};

// Issue #2183: single pure restamp helper for CacheEntryVersionStamp.
// All production store / partial / cascade / AOT success paths must
// funnel here (or via IRCacheEntry::stamp_version which calls this).
inline void restamp_cache_entry(CacheEntryVersionStamp& s, std::uint64_t mut, std::uint64_t bridge,
                                std::uint64_t defuse, std::uint64_t soa_gen = 0) noexcept {
    s.mutation_count = mut;
    s.bridge_epoch = bridge;
    s.defuse_version = defuse;
    s.soa_generation = soa_gen;
}

// Reason bitflags for should_relower observability (optional out-param).
inline constexpr std::uint32_t kRelowerDirty = 1u << 0;
inline constexpr std::uint32_t kRelowerSourceHash = 1u << 1;
inline constexpr std::uint32_t kRelowerMutationDrift = 1u << 2;
inline constexpr std::uint32_t kRelowerBridgeEpoch = 1u << 3;
inline constexpr std::uint32_t kRelowerDefuseVersion = 1u << 4;
inline constexpr std::uint32_t kRelowerSoaGeneration = 1u << 5; // Issue #2111

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
//   - soa_generation advanced (Issue #2111; dirty may be false)
//
// Pure: same inputs → same output. reasons_out optional bitmask.
// current_soa_generation: live SoA fence (0 = skip generation check).
[[nodiscard]] inline bool should_relower(std::size_t source_hash, std::size_t cached_source_hash,
                                         bool dirty, const CacheEntryVersionStamp& stamp,
                                         std::uint64_t current_mutation_count,
                                         std::uint64_t current_bridge_epoch,
                                         std::uint64_t current_defuse_version = 0,
                                         std::uint32_t* reasons_out = nullptr,
                                         std::uint64_t current_soa_generation = 0) noexcept {
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
    // Issue #2111: generation fence — cached_gen < live_gen forces re-lower
    // even when dirty==false and hashes match (silent-stale under compact).
    if (current_soa_generation != 0 && stamp.soa_generation < current_soa_generation)
        reasons |= kRelowerSoaGeneration;
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

    // Issue #2133: true when affected_instrs carries precise work.
    [[nodiscard]] bool has_instr_precision() const noexcept { return !affected_instrs.empty(); }
    // Eligible for instr-only relower/pass when under partial threshold.
    [[nodiscard]] bool instr_level_eligible(std::size_t thr) const noexcept {
        return has_instr_precision() && thr > 0 && affected_instrs.size() < thr;
    }
    // Unmapped AST ratio in basis points (0..10000). 0 if no visit.
    [[nodiscard]] std::uint64_t unmapped_ratio_bp() const noexcept {
        if (ast_nodes_visited == 0)
            return 0;
        return (static_cast<std::uint64_t>(unmapped_ast_nodes) * 10000u) /
               static_cast<std::uint64_t>(ast_nodes_visited);
    }
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

// Issue #2179: cross-function instruction-level impact scope
// (refine #2109). When the mutated root's define has callers in
// node_dep_graph_ (encoded fn-name slots), scan each caller's IR
// for Call instructions whose callee matches the mutated_name and
// emit precise (caller_func, caller_block, caller_instr) into
// affected_instrs. Same block also goes into affected_blocks.
//
// AC1: precise call-site instrs in callers (not whole nested lambdas
// that do not free-ref the name).
// AC4: empty irs OR empty mutated_name OR empty node_dep_graph_ →
// behavior identical to today (local subtree only — no cross-fn
// fan-out). Single-overload callers are unchanged.
inline ImpactScope compute_impact_scope(
    const aura::ast::FlatAST& flat, aura::ast::NodeId root, const SourceToIrMap& source_to_ir_map,
    const std::unordered_map<std::string, std::size_t, aura::core::TransparentStringHash,
                             std::equal_to<>>& ir_cache_index,
    const std::vector<aura::ir::IRFunction>& irs,
    const aura::compiler::dirty::DepGraph& node_dep_graph, const std::string& mutated_name) {
    // Run the single-function walk first.
    auto result = compute_impact_scope(flat, root, source_to_ir_map, ir_cache_index);
    // AC4 guard: empty irs / missing mutated_name / missing dep edges
    // → identical to today (skip cross-fn fan-out entirely).
    if (irs.empty() || mutated_name.empty() || node_dep_graph.adj.empty()) {
        return result;
    }
    // Find mutated_name's slot via ir_cache_index; if not present,
    // there's no NodeId-encoding for it — fall back to silent skip.
    auto slot_it = ir_cache_index.find(mutated_name);
    if (slot_it == ir_cache_index.end())
        return result;
    const auto mutated_slot = static_cast<std::uint32_t>(slot_it->second);
    // encode_fn_node convention (dirty_propagation.ixx):
    //   kFnNodeTag = 0x80000000u
    //   mutated_nid = 0x80000000u | (mutated_slot & 0x7FFFFFFFu)
    constexpr aura::ast::NodeId kFnNodeTag = 0x80000000u;
    const auto mutated_nid = static_cast<aura::ast::NodeId>(
        kFnNodeTag | (static_cast<aura::ast::NodeId>(mutated_slot) & 0x7FFFFFFFu));
    // Look up callers of mutated fn via node_dep_graph.dependents.
    const auto* callers = node_dep_graph.dependents(mutated_nid);
    if (!callers || callers->empty())
        return result;
    // For each caller fn (NodeId-encoded slot), scan its IR for
    // Call instructions whose operand[0] (callee slot) resolves to
    // the mutated function. Emit (caller_func, caller_block,
    // caller_instr) into affected_instrs.
    std::unordered_set<std::uint64_t> seen_blocks_cf;
    std::unordered_set<std::uint64_t> seen_instrs_cf;
    for (const auto& caller_nid : *callers) {
        const auto caller_slot =
            static_cast<std::uint32_t>(static_cast<uint32_t>(caller_nid) & 0x7FFFFFFFu);
        if (caller_slot == 0 || caller_slot > irs.size())
            continue;
        const auto caller_func_idx = static_cast<std::size_t>(caller_slot - 1u);
        const auto& fn = irs[caller_func_idx];
        for (std::uint32_t bi = 0; bi < static_cast<std::uint32_t>(fn.blocks.size()); ++bi) {
            const auto& blk = fn.blocks[bi];
            for (std::uint32_t ii = 0; ii < static_cast<std::uint32_t>(blk.instructions.size());
                 ++ii) {
                const auto& ins = blk.instructions[ii];
                // Only Call instructions can reference a callee function.
                if (ins.opcode != aura::ir::IROpcode::Call)
                    continue;
                // Operand[0] is the callee slot — resolve to name via
                // ir_cache_index. If it matches mutated_name, this
                // call site is impacted by the mutation.
                if (ins.operands[0] >= irs.size())
                    continue;
                const auto& callee_fn = irs[ins.operands[0]];
                if (callee_fn.name != mutated_name)
                    continue;
                const auto bkey_cf = (static_cast<std::uint64_t>(caller_func_idx) << 32) |
                                     static_cast<std::uint64_t>(bi);
                if (seen_blocks_cf.insert(bkey_cf).second) {
                    result.affected_blocks.push_back({caller_func_idx, bi});
                }
                const auto ikey_cf = (static_cast<std::uint64_t>(caller_func_idx) << 40) |
                                     (static_cast<std::uint64_t>(bi) << 20) |
                                     static_cast<std::uint64_t>(ii);
                if (seen_instrs_cf.insert(ikey_cf).second) {
                    result.affected_instrs.push_back({caller_func_idx, bi, ii});
                    ++result.instr_level_hits;
                }
            }
        }
    }
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

// Issue #2244: Strict-mode hard-fail + rebuild on source_to_ir_map
// inconsistency (refine #2045 / #2045 helpers, complement #2193
// MapInconsistent fallback reason — this issue owns the
// correctness gate, not the reason enum).
//
// Two modes:
//   - Off (default, unit-test): rebuild runs + bumps diagnostic
//     counter, but does NOT hard-fail the next lookup (preserves
//     existing soft-path tests).
//   - Strict (production / multi-tenant sandbox): detect →
//     metric → full rebuild of the map → caller signals
//     hard_failed=true so the cascade can force full re-lower
//     (mark_all_blocks_dirty) instead of serving stale clean
//     blocks from a desynced reverse index.
enum class SourceToIrStrictMode : std::uint8_t {
    Off = 0,
    Strict = 1,
};

struct EnsureSourceToIrResult {
    bool was_consistent = true;
    std::size_t bad_entries = 0;
    bool rebuilt = false;
    bool hard_failed = false;
};

// AC3: zero extra cost on the happy (consistent) path — single
// count call (already O(map.size())) + early return before any
// rebuild. Inconsistent path: bump counters + rebuild + signal
// Strict-mode hard-fail so caller can force full re-lower.
[[nodiscard]] inline EnsureSourceToIrResult
ensure_source_to_ir_or_rebuild(const std::vector<aura::ir::IRFunction>& irs, SourceToIrMap& map,
                               SourceToIrStrictMode mode) noexcept {
    EnsureSourceToIrResult r;
    r.bad_entries = count_source_to_ir_map_inconsistencies(irs, map);
    r.was_consistent = (r.bad_entries == 0);
    if (r.was_consistent)
        return r;
    rebuild_source_to_ir_map_from_irs(irs, map);
    r.rebuilt = true;
    if (mode == SourceToIrStrictMode::Strict)
        r.hard_failed = true;
    return r;
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

// Issue #426 / #2032 / #2112: configurable partial-vs-full re-lower threshold.
// Default 8 (historical): 0 → skip, 1..(T-1) → partial, ≥T → full.
// Agents / tests may raise (bulk set-code) or lower (tiny AI edits).
// Issue #2112: after enough cost samples, threshold adapts within [4, 32]
// from measured partial vs full relower cost (cold-start stays at 8).
inline constexpr std::size_t kDefaultPartialRelowerThreshold = 8;
inline constexpr std::size_t kAdaptivePartialRelowerMin = 4;
inline constexpr std::size_t kAdaptivePartialRelowerMax = 32;
// Cold-start: need both partial and full samples before adapting.
inline constexpr std::uint64_t kAdaptiveRelowerMinSamples = 8;

inline std::atomic<std::size_t>& partial_relower_threshold_atomic() noexcept {
    static std::atomic<std::size_t> thr{kDefaultPartialRelowerThreshold};
    return thr;
}

// Explicit override (set_partial_relower_threshold) freezes adaptive.
inline std::atomic<bool>& partial_relower_threshold_forced_atomic() noexcept {
    static std::atomic<bool> forced{false};
    return forced;
}

// Rolling cost history (process-wide; pure decision still reads only thr).
inline std::atomic<std::uint64_t>& partial_relower_cost_ns_sum_atomic() noexcept {
    static std::atomic<std::uint64_t> s{0};
    return s;
}
inline std::atomic<std::uint64_t>& partial_relower_cost_samples_atomic() noexcept {
    static std::atomic<std::uint64_t> s{0};
    return s;
}
inline std::atomic<std::uint64_t>& full_relower_cost_ns_sum_atomic() noexcept {
    static std::atomic<std::uint64_t> s{0};
    return s;
}
inline std::atomic<std::uint64_t>& full_relower_cost_samples_atomic() noexcept {
    static std::atomic<std::uint64_t> s{0};
    return s;
}

// Issue #2127: last workload-adaptive decision snapshot (declared early
// so reset_partial_relower_threshold_for_test can clear them).
inline std::atomic<std::size_t>& adaptive_effective_threshold_atomic() noexcept {
    static std::atomic<std::size_t> t{kDefaultPartialRelowerThreshold};
    return t;
}
inline std::atomic<std::uint32_t>& adaptive_last_reason_atomic() noexcept {
    static std::atomic<std::uint32_t> r{0};
    return r;
}
inline std::atomic<std::uint64_t>& adaptive_partial_decision_total_atomic() noexcept {
    static std::atomic<std::uint64_t> n{0};
    return n;
}
inline std::atomic<std::uint64_t>& adaptive_full_decision_total_atomic() noexcept {
    static std::atomic<std::uint64_t> n{0};
    return n;
}
inline std::atomic<std::uint64_t>& adaptive_deopt_adjust_total_atomic() noexcept {
    static std::atomic<std::uint64_t> n{0};
    return n;
}
inline std::atomic<std::uint64_t>& adaptive_density_adjust_total_atomic() noexcept {
    static std::atomic<std::uint64_t> n{0};
    return n;
}

// Issue #2190: StormLevel gate on partial vs full relower.
// Bits match HotUpdateRegistry::StormLevel (None=0, Shape=1, Global=2, Both=3).
inline constexpr std::uint8_t kStormLevelNone = 0;
inline constexpr std::uint8_t kStormLevelShape = 1;
inline constexpr std::uint8_t kStormLevelGlobal = 2; // bit
inline constexpr std::uint8_t kStormLevelBoth = 3;

inline std::atomic<std::uint64_t>& partial_relower_storm_gate_consult_total_atomic() noexcept {
    static std::atomic<std::uint64_t> n{0};
    return n;
}
inline std::atomic<std::uint64_t>& partial_relower_storm_forced_full_total_atomic() noexcept {
    static std::atomic<std::uint64_t> n{0};
    return n;
}

[[nodiscard]] inline std::size_t get_partial_relower_threshold() noexcept {
    return partial_relower_threshold_atomic().load(std::memory_order_relaxed);
}

[[nodiscard]] inline bool partial_relower_threshold_is_forced() noexcept {
    return partial_relower_threshold_forced_atomic().load(std::memory_order_relaxed);
}

// Clamp into adaptive band [4, 32]; 0 → default (never all-skip).
[[nodiscard]] inline std::size_t clamp_partial_relower_threshold(std::size_t t) noexcept {
    if (t == 0)
        return kDefaultPartialRelowerThreshold;
    if (t < kAdaptivePartialRelowerMin)
        return kAdaptivePartialRelowerMin;
    if (t > kAdaptivePartialRelowerMax)
        return kAdaptivePartialRelowerMax;
    return t;
}

// Explicit override for tests / Agent policy (freezes adaptive until reset).
inline void set_partial_relower_threshold(std::size_t t) noexcept {
    if (t == 0)
        t = 1; // legacy: 0 → 1 for "always full at 1 dirty"
    // Agent/tests may set outside [4,32] intentionally (e.g. thr=1); allow.
    partial_relower_threshold_atomic().store(t, std::memory_order_relaxed);
    partial_relower_threshold_forced_atomic().store(true, std::memory_order_relaxed);
}

// Average partial relower cost in ns (0 if no samples).
[[nodiscard]] inline std::uint64_t avg_partial_relower_cost_ns() noexcept {
    const auto n = partial_relower_cost_samples_atomic().load(std::memory_order_relaxed);
    if (n == 0)
        return 0;
    return partial_relower_cost_ns_sum_atomic().load(std::memory_order_relaxed) / n;
}

[[nodiscard]] inline std::uint64_t avg_full_relower_cost_ns() noexcept {
    const auto n = full_relower_cost_samples_atomic().load(std::memory_order_relaxed);
    if (n == 0)
        return 0;
    return full_relower_cost_ns_sum_atomic().load(std::memory_order_relaxed) / n;
}

// Basis points: 10000 * avg_full / avg_partial (0 if missing samples).
// >10000 means full is more expensive than partial on average.
[[nodiscard]] inline std::uint64_t partial_vs_full_win_ratio_bp() noexcept {
    const auto ap = avg_partial_relower_cost_ns();
    const auto af = avg_full_relower_cost_ns();
    if (ap == 0)
        return 0;
    return (af * 10000ull) / ap;
}

// Issue #2112: recompute threshold from cost history when not forced.
// Cold-start (insufficient samples) leaves threshold at default 8.
inline void maybe_adapt_partial_relower_threshold() noexcept {
    if (partial_relower_threshold_forced_atomic().load(std::memory_order_relaxed))
        return;
    const auto ps = partial_relower_cost_samples_atomic().load(std::memory_order_relaxed);
    const auto fs = full_relower_cost_samples_atomic().load(std::memory_order_relaxed);
    if (ps < kAdaptiveRelowerMinSamples || fs < kAdaptiveRelowerMinSamples)
        return; // AC1: no cold-start cliff — stay at default until enough data
    const auto ap = avg_partial_relower_cost_ns();
    const auto af = avg_full_relower_cost_ns();
    if (ap == 0)
        return;
    auto thr = get_partial_relower_threshold();
    // If full is much more expensive than partial → prefer partial longer (↑ thr).
    // If full is cheaper relative to partial → prefer full sooner (↓ thr).
    if (af > ap * 3)
        thr = thr + 1;
    else if (af < ap * 2 && thr > kAdaptivePartialRelowerMin)
        thr = thr - 1;
    thr = clamp_partial_relower_threshold(thr);
    partial_relower_threshold_atomic().store(thr, std::memory_order_relaxed);
}

// Record measured cost of a partial (or full) relower attempt.
inline void note_partial_relower_cost_ns(std::uint64_t ns) noexcept {
    partial_relower_cost_ns_sum_atomic().fetch_add(ns, std::memory_order_relaxed);
    partial_relower_cost_samples_atomic().fetch_add(1, std::memory_order_relaxed);
    maybe_adapt_partial_relower_threshold();
}

inline void note_full_relower_cost_ns(std::uint64_t ns) noexcept {
    full_relower_cost_ns_sum_atomic().fetch_add(ns, std::memory_order_relaxed);
    full_relower_cost_samples_atomic().fetch_add(1, std::memory_order_relaxed);
    maybe_adapt_partial_relower_threshold();
}

// Test hook: inject synthetic cost samples. Does not clear an explicit
// threshold force (AC4 sticky override); call
// reset_partial_relower_threshold_for_test first to re-enable adaptive.
inline void inject_adaptive_relower_cost_samples_for_test(std::uint64_t partial_ns,
                                                          std::uint64_t full_ns,
                                                          std::uint64_t n) noexcept {
    for (std::uint64_t i = 0; i < n; ++i) {
        note_partial_relower_cost_ns(partial_ns);
        note_full_relower_cost_ns(full_ns);
    }
}

inline void reset_partial_relower_threshold_for_test() noexcept {
    partial_relower_threshold_forced_atomic().store(false, std::memory_order_relaxed);
    partial_relower_cost_ns_sum_atomic().store(0, std::memory_order_relaxed);
    partial_relower_cost_samples_atomic().store(0, std::memory_order_relaxed);
    full_relower_cost_ns_sum_atomic().store(0, std::memory_order_relaxed);
    full_relower_cost_samples_atomic().store(0, std::memory_order_relaxed);
    partial_relower_threshold_atomic().store(kDefaultPartialRelowerThreshold,
                                             std::memory_order_relaxed);
    // Issue #2127: clear workload-adaptive decision snapshot.
    adaptive_effective_threshold_atomic().store(kDefaultPartialRelowerThreshold,
                                                std::memory_order_relaxed);
    adaptive_last_reason_atomic().store(0, std::memory_order_relaxed);
    adaptive_partial_decision_total_atomic().store(0, std::memory_order_relaxed);
    adaptive_full_decision_total_atomic().store(0, std::memory_order_relaxed);
    adaptive_deopt_adjust_total_atomic().store(0, std::memory_order_relaxed);
    adaptive_density_adjust_total_atomic().store(0, std::memory_order_relaxed);
    // Issue #2190: clear StormLevel partial-gate metrics.
    partial_relower_storm_gate_consult_total_atomic().store(0, std::memory_order_relaxed);
    partial_relower_storm_forced_full_total_atomic().store(0, std::memory_order_relaxed);
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
// Issue #2127: production cascade sites prefer
// decide_workload_adaptive_partial_relower (deopt + density). This
// overload remains the #2032 / #2112 base (reads process thr only).
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

// ── Issue #2190: StormLevel gate on partial vs full ──────────────
// HotUpdateRegistry::StormLevel (via aura_hot_update_current_storm_level):
//   None=0, Shape=1, Global=2, Both=3.
// When the Global bit is set (Global or Both), prefer full relower to
// avoid partial-fail→full thrash under process-wide deopt storm.
// Shape-only does NOT force full (align #2172 AC1).
// set_partial_relower_threshold / env still apply; this is an
// *additional* gate on top of #2032/#2112/#2127 threshold logic.
//
// Pure should_partial_relower remains threshold-only for unit tests.
// Production cascade / DirtyAware / query surfaces use the storm-aware
// helpers below (or consult_workload_adaptive_partial_ which applies
// the same gate).

[[nodiscard]] inline bool storm_level_has_global() noexcept {
    return (aura_hot_update_current_storm_level() & kStormLevelGlobal) != 0;
}

// Apply Global-storm gate to a pre-storm partial decision.
// Always bumps partial_relower_storm_gate_consult_total.
// Under Global: bumps forced_full and returns false (prefer full).
// Shape-only / None: returns want_partial_without_storm unchanged.
[[nodiscard]] inline bool
apply_partial_relower_storm_gate(bool want_partial_without_storm) noexcept {
    partial_relower_storm_gate_consult_total_atomic().fetch_add(1, std::memory_order_relaxed);
    if (storm_level_has_global()) {
        partial_relower_storm_forced_full_total_atomic().fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return want_partial_without_storm;
}

// Drop-in production consult: threshold decision + StormLevel Global gate.
// Shape-only leaves the pure threshold result intact.
[[nodiscard]] inline bool should_partial_relower_storm_aware(std::size_t dirty_count) noexcept {
    return apply_partial_relower_storm_gate(should_partial_relower(dirty_count));
}

// ── Issue #2127: workload / deopt / density adaptive threshold ──
// Builds on #2112 cost history (base thr) + HotUpdateRegistry deopt
// storm + dirty density so micro-edits prefer partial and bulk /
// storm windows prefer full sooner (avoid partial-fail→full double cost).

// Reason bitmask for Agent dashboards (query:incremental-relower-stats).
inline constexpr std::uint32_t kAdaptiveReasonBase = 1u << 0;
inline constexpr std::uint32_t kAdaptiveReasonDeoptStorm = 1u << 1;  // thr ↓
inline constexpr std::uint32_t kAdaptiveReasonHighDensity = 1u << 2; // thr ↓
inline constexpr std::uint32_t kAdaptiveReasonLowDeopt = 1u << 3;    // thr ↑
inline constexpr std::uint32_t kAdaptiveReasonLowDensity = 1u << 4;  // thr ↑
inline constexpr std::uint32_t kAdaptiveReasonForced = 1u << 5;
inline constexpr std::uint32_t kAdaptiveReasonPartial = 1u << 6;
inline constexpr std::uint32_t kAdaptiveReasonFull = 1u << 7;
inline constexpr std::uint32_t kAdaptiveReasonSkipClean = 1u << 8;

struct AdaptiveRelowerPolicy {
    std::size_t base = kDefaultPartialRelowerThreshold;

    // deopt_window_count: deopts in current sliding window.
    // deopt_storm_threshold: storm trip point (0 = deopt signal disabled).
    // deopt_storm_active: throttle / shape storm already tripped.
    // dirty_density_bp: 10000 * dirty / total_blocks (0 if unknown).
    // When forced, base is used as-is (no deopt/density delta).
    [[nodiscard]] std::size_t effective(std::uint64_t deopt_window_count,
                                        std::uint64_t deopt_storm_threshold,
                                        bool deopt_storm_active, std::uint32_t dirty_density_bp,
                                        bool forced,
                                        std::uint32_t* out_reason = nullptr) const noexcept {
        std::uint32_t reason = kAdaptiveReasonBase;
        auto thr = base == 0 ? kDefaultPartialRelowerThreshold : base;
        if (forced) {
            reason |= kAdaptiveReasonForced;
            if (out_reason)
                *out_reason = reason;
            return thr; // Agent override: no further adaptation
        }
        // Density: high (≥50%) → prefer full sooner (↓ thr by 2).
        // Low (<15%) with some dirt → prefer partial longer (↑ thr by 1).
        if (dirty_density_bp >= 5000) {
            thr = thr > kAdaptivePartialRelowerMin + 1 ? thr - 2 : kAdaptivePartialRelowerMin;
            reason |= kAdaptiveReasonHighDensity;
        } else if (dirty_density_bp > 0 && dirty_density_bp < 1500) {
            thr = thr + 1;
            reason |= kAdaptiveReasonLowDensity;
        }
        // Deopt storm: active or window ≥ half threshold → prefer full (↓ thr).
        // Quiet window (0 or <10% of thr) → prefer partial (↑ thr).
        if (deopt_storm_active ||
            (deopt_storm_threshold > 0 && deopt_window_count * 2 >= deopt_storm_threshold)) {
            thr = thr > kAdaptivePartialRelowerMin + 1 ? thr - 2 : kAdaptivePartialRelowerMin;
            reason |= kAdaptiveReasonDeoptStorm;
        } else if (deopt_storm_threshold == 0 || deopt_window_count == 0 ||
                   (deopt_storm_threshold > 0 && deopt_window_count * 10 < deopt_storm_threshold)) {
            thr = thr + 1;
            reason |= kAdaptiveReasonLowDeopt;
        }
        thr = clamp_partial_relower_threshold(thr);
        if (out_reason)
            *out_reason = reason;
        return thr;
    }
};

struct AdaptiveRelowerDecision {
    std::size_t effective_threshold = kDefaultPartialRelowerThreshold;
    bool want_partial = false;
    std::uint32_t reason_bits = 0;
    std::uint32_t dirty_density_bp = 0;
};

[[nodiscard]] inline std::size_t get_effective_partial_relower_threshold() noexcept {
    return adaptive_effective_threshold_atomic().load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t get_last_adaptive_relower_reason() noexcept {
    return adaptive_last_reason_atomic().load(std::memory_order_relaxed);
}

// Issue #2127: workload-aware partial/full decision.
// total_blocks=0 → density unknown (0 bp); base thr from #2032/#2112.
// Pure aside from process-wide last-decision atomics (metrics).
[[nodiscard]] inline AdaptiveRelowerDecision decide_workload_adaptive_partial_relower(
    std::size_t dirty_count, std::size_t total_blocks, std::uint64_t deopt_window_count = 0,
    std::uint64_t deopt_storm_threshold = 0, bool deopt_storm_active = false) noexcept {
    AdaptiveRelowerDecision d;
    if (total_blocks > 0 && dirty_count > 0) {
        d.dirty_density_bp =
            static_cast<std::uint32_t>((static_cast<std::uint64_t>(dirty_count) * 10000ull) /
                                       static_cast<std::uint64_t>(total_blocks));
        if (d.dirty_density_bp > 10000)
            d.dirty_density_bp = 10000;
    }
    AdaptiveRelowerPolicy pol;
    pol.base = get_partial_relower_threshold();
    const bool forced = partial_relower_threshold_is_forced();
    std::uint32_t reason = 0;
    d.effective_threshold = pol.effective(deopt_window_count, deopt_storm_threshold,
                                          deopt_storm_active, d.dirty_density_bp, forced, &reason);
    if (dirty_count == 0) {
        d.want_partial = false;
        reason |= kAdaptiveReasonSkipClean;
    } else if (dirty_count < d.effective_threshold) {
        d.want_partial = true;
        reason |= kAdaptiveReasonPartial;
        adaptive_partial_decision_total_atomic().fetch_add(1, std::memory_order_relaxed);
    } else {
        d.want_partial = false;
        reason |= kAdaptiveReasonFull;
        adaptive_full_decision_total_atomic().fetch_add(1, std::memory_order_relaxed);
    }
    if (reason & kAdaptiveReasonDeoptStorm)
        adaptive_deopt_adjust_total_atomic().fetch_add(1, std::memory_order_relaxed);
    if ((reason & kAdaptiveReasonHighDensity) || (reason & kAdaptiveReasonLowDensity))
        adaptive_density_adjust_total_atomic().fetch_add(1, std::memory_order_relaxed);
    d.reason_bits = reason;
    adaptive_effective_threshold_atomic().store(d.effective_threshold, std::memory_order_relaxed);
    adaptive_last_reason_atomic().store(reason, std::memory_order_relaxed);
    return d;
}

// Convenience: same signals as decide_*, for drop-in should_partial sites.
// Does NOT apply StormLevel Global force-full (#2190) — use
// should_partial_relower_workload_storm_aware for production.
[[nodiscard]] inline bool should_partial_relower_workload(
    std::size_t dirty_count, std::size_t total_blocks = 0, std::uint64_t deopt_window_count = 0,
    std::uint64_t deopt_storm_threshold = 0, bool deopt_storm_active = false) noexcept {
    return decide_workload_adaptive_partial_relower(dirty_count, total_blocks, deopt_window_count,
                                                    deopt_storm_threshold, deopt_storm_active)
        .want_partial;
}

// Issue #2190: workload adaptive + StormLevel Global force-full gate.
[[nodiscard]] inline bool should_partial_relower_workload_storm_aware(
    std::size_t dirty_count, std::size_t total_blocks = 0, std::uint64_t deopt_window_count = 0,
    std::uint64_t deopt_storm_threshold = 0, bool deopt_storm_active = false) noexcept {
    return apply_partial_relower_storm_gate(should_partial_relower_workload(
        dirty_count, total_blocks, deopt_window_count, deopt_storm_threshold, deopt_storm_active));
}

// ── Issue #2113: incremental soundness oracle (partial ≡ full) ──
// Debug/self-evo confidence: after partial re-lower, optionally
// re-lower fully and compare IR. Zero overhead when disabled
// (callers gate on incremental_soundness_enabled()).
//
// Enable:
//   - Compile with -DAURA_INCREMENTAL_SOUNDNESS, or
//   - Debug builds (!NDEBUG) default auto-on, or
//   - Runtime: set_incremental_soundness_mode(1) from tests
// Disable always: set_incremental_soundness_mode(2)
// Auto: set_incremental_soundness_mode(0)

inline std::atomic<int>& incremental_soundness_mode_atomic() noexcept {
    static std::atomic<int> mode{0}; // 0=auto, 1=on, 2=off
    return mode;
}

inline void set_incremental_soundness_mode(int mode) noexcept {
    // 0 auto, 1 force on, 2 force off
    incremental_soundness_mode_atomic().store(mode, std::memory_order_relaxed);
}

[[nodiscard]] inline int get_incremental_soundness_mode() noexcept {
    return incremental_soundness_mode_atomic().load(std::memory_order_relaxed);
}

[[nodiscard]] inline bool incremental_soundness_enabled() noexcept {
    const int m = get_incremental_soundness_mode();
    if (m == 1)
        return true;
    if (m == 2)
        return false;
#if defined(AURA_INCREMENTAL_SOUNDNESS)
    return true;
#elif !defined(NDEBUG)
    return true;
#else
    return false;
#endif
}

// Process-wide oracle counters (always present; zero cost when unused).
inline std::atomic<std::uint64_t>& incremental_soundness_runs_atomic() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& incremental_soundness_ok_atomic() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& incremental_soundness_mismatch_atomic() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}

// Pure IR instruction equivalence — opcodes + operands + type/shape
// metadata. Ignores pure layout noise (none currently; source_ast kept
// because same AST lower must agree).
[[nodiscard]] inline bool ir_instruction_equivalent(const aura::ir::IRInstruction& a,
                                                    const aura::ir::IRInstruction& b) noexcept {
    if (a.opcode != b.opcode)
        return false;
    if (a.operands != b.operands)
        return false;
    if (a.type_id != b.type_id || a.shape_id != b.shape_id)
        return false;
    if (a.linear_ownership_state != b.linear_ownership_state)
        return false;
    if (a.adt_variant_id != b.adt_variant_id || a.narrow_evidence != b.narrow_evidence)
        return false;
    // source_ast_node_id: same lower of same AST should match; include for
    // stronger oracle (under-dirty keeps stale source stamps).
    if (a.source_ast_node_id != b.source_ast_node_id)
        return false;
    return true;
}

// Pure IR function equivalence (structure of blocks + instructions).
// Ignores function id / name / specialized_for (partial preserves id;
// full lower may assign temporary id 0).
[[nodiscard]] inline bool ir_function_equivalent(const aura::ir::IRFunction& a,
                                                 const aura::ir::IRFunction& b) noexcept {
    if (a.blocks.size() != b.blocks.size())
        return false;
    if (a.arg_count != b.arg_count || a.local_count != b.local_count)
        return false;
    if (a.variadic != b.variadic)
        return false;
    if (a.params.size() != b.params.size())
        return false;
    for (std::size_t i = 0; i < a.params.size(); ++i) {
        if (a.params[i] != b.params[i])
            return false;
    }
    // free_vars: order may differ after partial merge; compare as sets
    // only by size for soft check; require exact order when both empty
    // or equal size with matching multiset.
    if (a.free_vars.size() != b.free_vars.size())
        return false;
    for (std::size_t bi = 0; bi < a.blocks.size(); ++bi) {
        const auto& ba = a.blocks[bi];
        const auto& bb = b.blocks[bi];
        if (ba.instructions.size() != bb.instructions.size())
            return false;
        if (ba.successors.size() != bb.successors.size())
            return false;
        for (std::size_t s = 0; s < ba.successors.size(); ++s) {
            if (ba.successors[s] != bb.successors[s])
                return false;
        }
        for (std::size_t ii = 0; ii < ba.instructions.size(); ++ii) {
            if (!ir_instruction_equivalent(ba.instructions[ii], bb.instructions[ii]))
                return false;
        }
    }
    return true;
}

// Alias name used in issue design docs.
[[nodiscard]] inline bool ir_equivalent(const aura::ir::IRFunction& a,
                                        const aura::ir::IRFunction& b) noexcept {
    return ir_function_equivalent(a, b);
}

// Run oracle compare: record metrics. Returns true if equivalent.
// Does not abort unless AURA_INCREMENTAL_SOUNDNESS_HARD is defined.
[[nodiscard]] inline bool check_incremental_soundness(const aura::ir::IRFunction& partial,
                                                      const aura::ir::IRFunction& full) noexcept {
    incremental_soundness_runs_atomic().fetch_add(1, std::memory_order_relaxed);
    const bool ok = ir_equivalent(partial, full);
    if (ok) {
        incremental_soundness_ok_atomic().fetch_add(1, std::memory_order_relaxed);
    } else {
        incremental_soundness_mismatch_atomic().fetch_add(1, std::memory_order_relaxed);
#if defined(AURA_INCREMENTAL_SOUNDNESS_HARD)
        assert(ok && "incremental soundness violation: partial IR != full IR (#2113)");
#endif
    }
    return ok;
}

// Intentional under-dirty injection for tests: corrupt a single opcode
// so oracle must report mismatch (AC1).
inline void inject_soundness_under_dirty_for_test(aura::ir::IRFunction& fn) noexcept {
    if (fn.blocks.empty() || fn.blocks[0].instructions.empty())
        return;
    // Flip opcode to a different op (or ConstI64↔Return) without resizing.
    auto& inst = fn.blocks[0].instructions[0];
    using aura::ir::IROpcode;
    if (inst.opcode == IROpcode::ConstI64)
        inst.opcode = IROpcode::Return;
    else
        inst.opcode = IROpcode::ConstI64;
}

// Reset process-wide mode + counters (tests only).
inline void reset_incremental_soundness_for_test() noexcept {
    set_incremental_soundness_mode(0);
    incremental_soundness_runs_atomic().store(0, std::memory_order_relaxed);
    incremental_soundness_ok_atomic().store(0, std::memory_order_relaxed);
    incremental_soundness_mismatch_atomic().store(0, std::memory_order_relaxed);
}

} // namespace aura::compiler
