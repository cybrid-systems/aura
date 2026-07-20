// workspace_epoch.hh — Issue #1964 cycle 2a
// Unified workspace epoch counter. Consolidates 5 legacy counters:
//   - bridge_epoch      (Worker + Closure cache invalidation, 602 refs)
//   - mutation_epoch    (FlatAST global mutation epoch,        101 refs)
//   - subtree_generation_ (per-top-level-Define gen,           ~80 refs)
//   - wrap_epoch        (per-workspace wrap tracker,            ~20 refs)
//   - generation_       (AST workspace epoch, 1-indexed,       ~44 refs)
//
// Cycle 2a: type definition + per-kind atomic storage shim.
// Cycles 2b/2c/2d: collapse legacy counters into per-workspace
// `WorkspaceEpoch` instance + delete the legacy fields.
//
// All counter semantics are preserved (mutation/bridge/subtree/wrap/
// generation have distinct invariants per the AC). The unified type
// provides a single vocabulary + a single linter entry point so future
// cycle work is mechanical: each legacy field maps to a `WorkspaceEpoch`
// member of the appropriate kind, and the linter tracks migration
// progress.
//
// Not thread-safe by itself — caller (FlatAST::bump_generation,
// Worker::bump_bridge_epoch, etc.) is responsible for atomicity per
// the existing legacy counter semantics.

#ifndef AURA_CORE_WORKSPACE_EPOCH_HH
#define AURA_CORE_WORKSPACE_EPOCH_HH

#include <atomic>
#include <cstdint>

namespace aura::core {

// Kinds of epoch tracked per workspace. Each kind maps to one of the
// 5 legacy counters. Order is stable (used by serialization /
// observability snapshots); append new kinds at the END to preserve
// wire-format compatibility with prior snapshots.
enum class WorkspaceEpochKind : std::uint8_t {
    Mutation = 0,   // Formerly mutation_epoch_  (FlatAST global)
    Bridge = 1,     // Formerly g_bridge_epoch_  (Worker + Closure cache)
    Subtree = 2,    // Formerly subtree_gen_[id] (per-top-level-Define)
    Wrap = 3,       // Formerly wrap_epoch_      (generation_ wrap tracker)
    Generation = 4, // Formerly generation_      (AST workspace epoch)
};

// Sentinel "unset / legacy" epoch (zero). Matches the convention used
// by all 5 legacy counters: a captured epoch of 0 means "not stamped /
// legacy ref" and is treated as fresh.
inline constexpr std::uint64_t kWorkspaceEpochUnset = 0;

// Unified counter value. Not thread-safe on its own — the underlying
// storage is an atomic (one per kind) managed by the legacy call sites
// during cycles 2b/2c/2d migration.
struct WorkspaceEpoch {
    WorkspaceEpochKind kind = WorkspaceEpochKind::Mutation;
    std::uint64_t value = kWorkspaceEpochUnset;

    constexpr WorkspaceEpoch() noexcept = default;
    constexpr WorkspaceEpoch(WorkspaceEpochKind k, std::uint64_t v) noexcept
        : kind(k)
        , value(v) {}

    [[nodiscard]] constexpr bool is_unset() const noexcept { return value == kWorkspaceEpochUnset; }

    // Freshness check (cycle 2b invariant): an `other` epoch is fresh
    // against the current `cur` if `other.value == cur.value` or
    // `other.value == 0` (legacy / unset). Matches the legacy
    // `validate_mutation_id` / `epoch_fence_ok` semantics in
    // provenance_tracker.hh.
    [[nodiscard]] static constexpr bool is_fresh(std::uint64_t captured,
                                                 std::uint64_t current) noexcept {
        if (captured == kWorkspaceEpochUnset)
            return true; // unset / legacy
        return captured == current;
    }
};

// Process-wide per-kind atomic storage. Migration shim: cycle 2a
// declares the atomics here; cycles 2b/2c/2d move them to the
// per-workspace owner (FlatAST for Mutation/Subtree/Wrap/Generation,
// Worker for Bridge) and delete the legacy fields. The legacy
// counters keep their semantics (separate atomics per kind) until
// each migration round.
//
// Thread-safety: atomic with relaxed memory order, matching the
// legacy counter semantics (these are observability + freshness
// tracking counters, not memory-order fences).
inline std::atomic<std::uint64_t>& g_workspace_epoch_storage(WorkspaceEpochKind kind) noexcept {
    static std::atomic<std::uint64_t> mutation{0};
    static std::atomic<std::uint64_t> bridge{0};
    static std::atomic<std::uint64_t> subtree{0};
    static std::atomic<std::uint64_t> wrap{0};
    static std::atomic<std::uint64_t> generation{0};
    switch (kind) {
        case WorkspaceEpochKind::Mutation:
            return mutation;
        case WorkspaceEpochKind::Bridge:
            return bridge;
        case WorkspaceEpochKind::Subtree:
            return subtree;
        case WorkspaceEpochKind::Wrap:
            return wrap;
        case WorkspaceEpochKind::Generation:
            return generation;
    }
    return mutation; // unreachable; suppresses -Wreturn-type
}

// Convenience accessors (avoid `g_workspace_epoch_storage(kind).fetch_add(...)`
// noise at call sites). Each is a one-liner that delegates to the
// per-kind atomic. Cycles 2b/2c/2d replace these with per-workspace
// member-function accessors on the owning type (FlatAST / Worker).
inline std::uint64_t load_workspace_epoch(WorkspaceEpochKind kind) noexcept {
    return g_workspace_epoch_storage(kind).load(std::memory_order_relaxed);
}

inline void store_workspace_epoch(WorkspaceEpochKind kind, std::uint64_t v) noexcept {
    g_workspace_epoch_storage(kind).store(v, std::memory_order_relaxed);
}

inline std::uint64_t fetch_add_workspace_epoch(WorkspaceEpochKind kind,
                                               std::uint64_t delta = 1) noexcept {
    return g_workspace_epoch_storage(kind).fetch_add(delta, std::memory_order_relaxed);
}

// ── Issue #1964 cycle 2b: mutation_epoch migration accessors ─────
//
// Replaces the legacy `service.ixx::mutation_epoch_` field with a
// WorkspaceEpoch-canonical counter. All reads of "the current
// mutation epoch" across the codebase should go through
// `current_mutation_epoch()`; all writes should go through
// `bump_mutation_epoch()`. Cycle 2b ships the API + service.ixx
// migration; cycle 2d completes the legacy-field deletion in
// service.ixx itself (after consumers are migrated).
//
// The function-pointer indirection via
// `shape_jit_pass::g_mutation_epoch_fn` is no longer required once
// consumers call `current_mutation_epoch()` directly; cycle 2d will
// remove that indirection.

[[nodiscard]] inline std::uint64_t current_mutation_epoch() noexcept {
    return load_workspace_epoch(WorkspaceEpochKind::Mutation);
}

inline void bump_mutation_epoch(std::uint64_t delta = 1) noexcept {
    fetch_add_workspace_epoch(WorkspaceEpochKind::Mutation, delta);
}

} // namespace aura::core

#endif // AURA_CORE_WORKSPACE_EPOCH_HH
