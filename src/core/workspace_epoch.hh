// workspace_epoch.hh — Issue #1964 cycle 2a–2d / Issue #2039
// Unified workspace epoch vocabulary + process-global storage for the
// two process-scoped epochs (Mutation + Bridge).
//
// ## Final ownership model (cycle 2d / #2039)
//
// | Kind         | Storage owner                         | Accessor surface              |
// |--------------|---------------------------------------|-------------------------------|
// | Mutation     | process-global atomic (this header)   | current_mutation_epoch / bump |
// | Bridge       | process-global atomic (this header)   | current_bridge_epoch / bump   |
// |              | dual-written to C g_current_bridge_epoch for runtime.c |
// | Generation   | per-FlatAST `generation_` (uint16)    | FlatAST::generation()         |
// | Wrap         | per-FlatAST `wrap_epoch_`             | FlatAST::wrap_epoch()         |
// | Subtree      | per-FlatAST `subtree_gen_[id]`        | FlatAST subtree helpers       |
// | node_gen_    | per-FlatAST parallel to generation_   | is_valid / make_ref           |
//
// Per-AST fields (Generation / Wrap / Subtree / node_gen_) intentionally
// do NOT migrate to the process-global atomics — each FlatAST instance
// tracks its own StableNodeRef validity. Mutation + Bridge are process-
// scoped (single CompilerService / AOT runtime) and live only here after
// cycle 2d: the legacy `CompilerService::mutation_epoch_` field is deleted.
//
// Linter: scripts/check_workspace_epoch_migration.py --strict must report 0
// remaining raw `mutation_epoch_` / dual-storage consumer violations.

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

// Convenience accessors. Mutation/Bridge use acquire/release so
// should_relower / dep_graph stale-edge reject / apply_closure see
// published bumps (legacy mutation_epoch_ used acq/rel).
inline std::uint64_t load_workspace_epoch(WorkspaceEpochKind kind) noexcept {
    return g_workspace_epoch_storage(kind).load(std::memory_order_acquire);
}

inline void store_workspace_epoch(WorkspaceEpochKind kind, std::uint64_t v) noexcept {
    g_workspace_epoch_storage(kind).store(v, std::memory_order_release);
}

inline std::uint64_t fetch_add_workspace_epoch(WorkspaceEpochKind kind,
                                               std::uint64_t delta = 1) noexcept {
    return g_workspace_epoch_storage(kind).fetch_add(delta, std::memory_order_acq_rel);
}

// Issue #2154: optional hook after Mutation epoch advances (capability
// grant_min_valid sliding window). Null = no-op. Installed by
// capability_model.hh when the retain-window policy is present — keeps
// this header free of capability includes (no cycle).
using MutationEpochBumpHook = void (*)(std::uint64_t new_epoch) noexcept;

[[nodiscard]] inline std::atomic<MutationEpochBumpHook>& g_mutation_epoch_bump_hook() noexcept {
    static std::atomic<MutationEpochBumpHook> h{nullptr};
    return h;
}

inline void set_mutation_epoch_bump_hook(MutationEpochBumpHook fn) noexcept {
    g_mutation_epoch_bump_hook().store(fn, std::memory_order_release);
}

inline void notify_mutation_epoch_bump(std::uint64_t new_epoch) noexcept {
    if (auto* fn = g_mutation_epoch_bump_hook().load(std::memory_order_acquire))
        fn(new_epoch);
}

// ── Mutation epoch (process-global; #1964 2b + #2039 2d) ─────
// Sole storage: g_workspace_epoch_storage(Mutation). The legacy
// CompilerService::mutation_epoch_ field is deleted in #2039.
//
// Issue #2149: **security provenance uses Mutation only** —
// CapabilityGrant::grant_epoch, EffectProvenance::epoch on the
// effect-check path, grant_min_valid_epoch fence, and SecurityEvent
// correlation. Do not use Bridge as a capability fence key.
//
// Issue #2154: bump notifies the optional grant-epoch retain-window hook.

[[nodiscard]] inline std::uint64_t current_mutation_epoch() noexcept {
    return load_workspace_epoch(WorkspaceEpochKind::Mutation);
}

inline void bump_mutation_epoch(std::uint64_t delta = 1) noexcept {
    if (delta == 0)
        return;
    const auto prev = fetch_add_workspace_epoch(WorkspaceEpochKind::Mutation, delta);
    notify_mutation_epoch_bump(prev + delta);
}

// ── Bridge epoch (process-global; #1964 2c + #2039 2d) ─────
// Canonical storage: WorkspaceEpoch::Bridge. C runtime
// (aura_get/set_current_bridge_epoch) dual-writes the same value
// for lib/runtime.c aura_closure_call. CompilerService::bridge_epoch()
// historically aliases Mutation for Cycle 1 lockstep; prefer these
// accessors for new code that wants the Bridge kind explicitly.
//
// Issue #2149: Bridge is AOT/JIT/closure freshness only — independent
// bumps must not flip capability allow/deny for a valid grant.

[[nodiscard]] inline std::uint64_t current_bridge_epoch() noexcept {
    return load_workspace_epoch(WorkspaceEpochKind::Bridge);
}

inline void bump_bridge_epoch(std::uint64_t delta = 1) noexcept {
    fetch_add_workspace_epoch(WorkspaceEpochKind::Bridge, delta);
}

// Keep Bridge kind aligned with Mutation when the service uses the
// Cycle-1 "shared counter" protocol (bump both in lockstep).
inline void bump_mutation_and_bridge_epochs(std::uint64_t delta = 1) noexcept {
    if (delta == 0)
        return;
    const auto prev = fetch_add_workspace_epoch(WorkspaceEpochKind::Mutation, delta);
    store_workspace_epoch(WorkspaceEpochKind::Bridge, prev + delta);
    notify_mutation_epoch_bump(prev + delta);
}

} // namespace aura::core

#endif // AURA_CORE_WORKSPACE_EPOCH_HH
