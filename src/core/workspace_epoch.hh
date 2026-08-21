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
// Linter: scripts/coverage/checks/check_workspace_epoch_migration.py --strict must report 0
// remaining raw `mutation_epoch_` / dual-storage consumer violations.

#ifndef AURA_CORE_WORKSPACE_EPOCH_HH
#define AURA_CORE_WORKSPACE_EPOCH_HH

#include <atomic>
#include <cstdint>
#include <cstdlib> // std::getenv — AURA_QUERY_EPOCH_STRICT (#2192)

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

// ── Issue #2192: QueryEpoch snapshot contract ─────────────────
// Agents bind a query result to a consistent workspace view:
//   mutation_epoch + FlatAST generation (+ optional bridge / workspace_id).
// Capture under shared workspace_mtx_ (after fence); finish re-checks
// before returning. Strict mode → stale when the workspace advanced.
//
// Interaction with MutationBoundary / atomic-batch:
//   - Outermost Guard takes exclusive workspace_mtx_ → queries block
//     (shared_lock) for the full mutate; no torn topology mid-query.
//   - Nested Guards / txn-dirty do not release the exclusive lock;
//     epoch bumps under exclusive stay invisible to concurrent queries
//     until the outermost Guard unlocks.
//   - query:last-epoch / query:query-epoch-stats expose the last capture
//     so Agents correlate "this query" with a later mutate decision.
//
// Agent contract (when is my query consistent with my last mutate?):
//   1. After mutate commits, note mutation_epoch / generation (or read
//      engine:metrics "query:query-epoch-stats" after a follow-up query).
//   2. Run query under normal shared lock (blocks while Guard is open).
//   3. Compare last-mutation-epoch / last-generation to post-mutate
//      values; equal ⇒ result matches that commit.
//   4. Strict: apply_production_audit_defaults (Issue #3075) turns
//      g_query_epoch_strict on; Soft/apply_dev leaves it off. Operators
//      may also set_query_epoch_strict(true) or AURA_QUERY_EPOCH_STRICT=1.
//      If epoch advances during the query body (or restamp-budget force
//      stale, #3041), the primitive returns query-epoch-stale.

struct QueryEpoch {
    std::uint64_t mutation_epoch = kWorkspaceEpochUnset;
    std::uint64_t generation = kWorkspaceEpochUnset; // FlatAST::generation()
    std::uint64_t bridge_epoch = kWorkspaceEpochUnset;
    std::uint32_t workspace_id = 0;

    [[nodiscard]] constexpr bool is_unset() const noexcept {
        // Both zero only means "never captured" when capture_total is 0;
        // after capture, mutation_epoch==0 is a real counter value.
        return mutation_epoch == kWorkspaceEpochUnset && generation == kWorkspaceEpochUnset;
    }

    // Fresh when both fields still equal the live counters.
    // Note: do NOT treat mutation_epoch==0 as legacy-unset here —
    // process-global mutation starts at 0 and 0 is a valid capture.
    [[nodiscard]] bool is_fresh(std::uint64_t cur_mutation,
                                std::uint64_t cur_generation) const noexcept {
        return mutation_epoch == cur_mutation && generation == cur_generation;
    }
};

// Process-global last-capture + strict + metrics (multi-fiber readable).
inline std::atomic<std::uint64_t>& g_last_query_mutation_epoch() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_last_query_generation() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_last_query_bridge_epoch() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint32_t>& g_last_query_workspace_id() noexcept {
    static std::atomic<std::uint32_t> v{0};
    return v;
}
// Issue #3075: static default is false (Soft / sandbox=off). Production
// defaults flip this via set_query_epoch_strict(true) — query_epoch_strict()
// stays one acquire (do NOT OR production_defaults_active; extra load on
// the Soft happy path).
inline constexpr int kQueryEpochProductionStrictIssue = 3075;
inline std::atomic<bool>& g_query_epoch_strict() noexcept {
    static std::atomic<bool> v{false};
    return v;
}
inline std::atomic<std::uint64_t>& g_query_epoch_capture_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_query_epoch_mismatch_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_query_epoch_stale_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}

// Issue #3041: production restamp-budget exceed forces the active
// QueryEpoch stale so Agents can poll after Guard exit (no wait for
// the next is_valid / refresh_if_stale). Soft never sets this.
inline constexpr int kRestampBudgetQueryEpochStaleIssue = 3041;
inline std::atomic<std::uint32_t>& g_query_epoch_forced_stale() noexcept {
    static std::atomic<std::uint32_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_restamp_budget_query_epoch_stale_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}

inline void set_query_epoch_strict(bool on) noexcept {
    g_query_epoch_strict().store(on, std::memory_order_release);
}

// One acquire. Issue #3075: production policy is applied at
// apply_production / apply_dev, not re-read here.
[[nodiscard]] inline bool query_epoch_strict() noexcept {
    return g_query_epoch_strict().load(std::memory_order_acquire);
}

// One-shot env bootstrap (AURA_QUERY_EPOCH_STRICT=1|true|yes).
inline void maybe_init_query_epoch_strict_from_env() noexcept {
    static std::atomic<bool> done{false};
    if (done.exchange(true, std::memory_order_acq_rel))
        return;
    if (const char* e = std::getenv("AURA_QUERY_EPOCH_STRICT")) {
        if (e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y')
            set_query_epoch_strict(true);
    }
}

// Capture snapshot (call under shared workspace_mtx_ or equivalent fence).
[[nodiscard]] inline QueryEpoch capture_query_epoch(std::uint64_t flat_generation,
                                                    std::uint32_t workspace_id = 0) noexcept {
    maybe_init_query_epoch_strict_from_env();
    QueryEpoch e;
    e.mutation_epoch = current_mutation_epoch();
    e.generation = flat_generation;
    e.bridge_epoch = current_bridge_epoch();
    e.workspace_id = workspace_id;
    g_last_query_mutation_epoch().store(e.mutation_epoch, std::memory_order_relaxed);
    g_last_query_generation().store(e.generation, std::memory_order_relaxed);
    g_last_query_bridge_epoch().store(e.bridge_epoch, std::memory_order_relaxed);
    g_last_query_workspace_id().store(e.workspace_id, std::memory_order_relaxed);
    g_query_epoch_capture_total().fetch_add(1, std::memory_order_relaxed);
    // New capture is consistent with the post-restamp world.
    g_query_epoch_forced_stale().store(0, std::memory_order_release);
    return e;
}

// Issue #3041: production budget-exceed → mark last QueryEpoch stale
// (if any capture exists) + bump the pollable counter. Lazy-align is
// unchanged (caller still enables it). Soft / unlimited must not call.
inline void force_query_epoch_stale_from_restamp_budget() noexcept {
    g_restamp_budget_query_epoch_stale_total().fetch_add(1, std::memory_order_relaxed);
    if (g_query_epoch_capture_total().load(std::memory_order_relaxed) == 0)
        return; // no active QueryEpoch
    if (g_query_epoch_forced_stale().exchange(1, std::memory_order_acq_rel) != 0)
        return; // already forced this window
    // Poison last generation so last_query_epoch().is_fresh fails
    // immediately (Agents poll query:query-epoch-stats after Guard).
    const auto last_gen = g_last_query_generation().load(std::memory_order_relaxed);
    g_last_query_generation().store(~last_gen, std::memory_order_relaxed);
    g_query_epoch_stale_total().fetch_add(1, std::memory_order_relaxed);
    g_query_epoch_mismatch_total().fetch_add(1, std::memory_order_relaxed);
}

// End-of-query check. Returns false only when strict mode is on AND
// mutation_epoch or generation advanced since capture (stale).
// Always bumps mismatch when inconsistent (even non-strict) for Agents.
[[nodiscard]] inline bool finish_query_epoch(const QueryEpoch& start,
                                             std::uint64_t flat_generation) noexcept {
    const auto cur_mut = current_mutation_epoch();
    const bool forced = g_query_epoch_forced_stale().load(std::memory_order_acquire) != 0;
    if (!forced && start.is_fresh(cur_mut, flat_generation))
        return true;
    g_query_epoch_mismatch_total().fetch_add(1, std::memory_order_relaxed);
    if (query_epoch_strict()) {
        g_query_epoch_stale_total().fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true; // non-strict: result still returned; metric records mismatch
}

// Last captured epoch (for query:last-epoch / stats surfaces).
[[nodiscard]] inline QueryEpoch last_query_epoch() noexcept {
    QueryEpoch e;
    e.mutation_epoch = g_last_query_mutation_epoch().load(std::memory_order_relaxed);
    e.generation = g_last_query_generation().load(std::memory_order_relaxed);
    e.bridge_epoch = g_last_query_bridge_epoch().load(std::memory_order_relaxed);
    e.workspace_id = g_last_query_workspace_id().load(std::memory_order_relaxed);
    return e;
}

// Test hook: reset metrics (not last snapshot).
inline void reset_query_epoch_metrics_for_test() noexcept {
    g_query_epoch_capture_total().store(0, std::memory_order_relaxed);
    g_query_epoch_mismatch_total().store(0, std::memory_order_relaxed);
    g_query_epoch_stale_total().store(0, std::memory_order_relaxed);
    g_query_epoch_forced_stale().store(0, std::memory_order_relaxed);
    g_restamp_budget_query_epoch_stale_total().store(0, std::memory_order_relaxed);
    set_query_epoch_strict(false);
}

// ── Issue #2933: first-class QueryResult binding (AI multi-round memory) ──
// Agents cache query matches across query → mutate → re-query loops without
// full rescans. A QueryResult binds:
//   - matches as (node_id, generation) pairs (StableNodeRef layout)
//   - QueryEpoch snapshot (mutation_epoch + generation + bridge + workspace_id)
//   - optional pin flag (SafePCVSpan-backed children_stable path sets pinned)
//
// EvalValue surface is a hash (schema-2933 / query-result-tag=1). Default
// query:* return remains a bare match list; opt-in via :as-query-result /
// :query-result #t (AC2 Soft regression green).
//
// is_fresh: re-checks epoch against live mutation_epoch + FlatAST generation.
// Fail-closed on wrap / gen mismatch unless pinned (pin keeps storage alive
// via SafePCVSpan keep-alive on the children_stable path; match gens still
// validated when a FlatAST is supplied to is_fresh_with_refs).

// Issue #3103: schema-2 QueryResultMatch carries the full StableNodeRef
// provenance that production multi-tenant / COW / wrap / concurrent fiber
// checks need to validate Agent multi-round memory. Fields:
//   node_id (4)                       — StableNodeRef.id (was the only field
//                                        in schema-1 layout-only matches)
//   tenant_id (4)                     — tenant isolation token
//   fiber_id (4)                      — fiber isolation token
//   mutation_id_at_capture (4)        — mutation epoch at capture time
//   generation (2)                    — StableNodeRef.gen
//   wrap_epoch (2)                    — StableNodeRef.wrap_epoch
//   cow_epoch_at_capture (2)          — StableNodeRef.cow_epoch_at_capture
//   boundary_pinned (1)               — SafePCVSpan pin path
//   reserved (1)                      — padding to align to 4
// Schema-1 readers (pre-#3103) ignore the trailing 18 bytes; schema-2
// readers check schema_marker == wrap_epoch != 0 (cheap discriminator) and
// gate validation on tenant_id / fiber_id / cow_epoch_at_capture /
// mutation_id_at_capture when set.
struct QueryResultMatch {
    std::uint32_t node_id = 0;
    std::uint32_t tenant_id = 0;
    std::uint32_t fiber_id = 0;
    std::uint32_t mutation_id_at_capture = 0;
    std::uint16_t generation = 0;
    std::uint16_t wrap_epoch = 0;
    std::uint16_t cow_epoch_at_capture = 0;
    std::uint8_t boundary_pinned = 0;
    std::uint8_t reserved = 0; // alignment pad (schema-version bit space)

    // Issue #3103: schema-2 marker. True iff the trailing provenance fields
    // were captured at capture time (via :as-query-result / :query-result #t).
    // Layout-only schema-1 matches leave wrap_epoch == 0 + cow_epoch == 0 +
    // tenant_id == 0, so this naturally returns false for them.
    // Issue #3231: reserved != 0 is the production schema-2 stamp even when
    // wrap/tenant/fiber/cow/mid are still 0 (single-tenant never-wrapped).
    [[nodiscard]] constexpr bool has_full_provenance() const noexcept {
        return wrap_epoch != 0 || cow_epoch_at_capture != 0 || tenant_id != 0 || fiber_id != 0 ||
               mutation_id_at_capture != 0 || reserved != 0;
    }
};

struct QueryResult {
    // Layout-only match table (no FlatAST dependency in this header).
    // Prefer StableNodeRef at call sites; pack id+gen here for EvalValue.
    // Max practical size is Agent-bounded; empty = no matches.
    static constexpr std::size_t kMaxInlineMatches = 64;
    QueryResultMatch matches[kMaxInlineMatches]{};
    std::uint16_t match_count = 0;
    QueryEpoch epoch{};
    bool pinned = false; // SafePCVSpan pin path (children_stable)

    [[nodiscard]] bool empty() const noexcept { return match_count == 0; }

    // Epoch-only freshness (no per-ref check). Fail-closed when mutation
    // or generation advanced since capture.
    [[nodiscard]] bool is_fresh(std::uint64_t cur_mutation,
                                std::uint64_t cur_generation) const noexcept {
        return epoch.is_fresh(cur_mutation, cur_generation);
    }

    // Convenience against process-global mutation + provided generation.
    [[nodiscard]] bool is_fresh_live(std::uint64_t flat_generation) const noexcept {
        return is_fresh(current_mutation_epoch(), flat_generation);
    }

    bool push_match(std::uint32_t node_id, std::uint16_t generation, std::uint16_t wrap_epoch = 0,
                    std::uint16_t cow_epoch_at_capture = 0, std::uint32_t tenant_id = 0,
                    std::uint32_t fiber_id = 0, std::uint32_t mutation_id_at_capture = 0,
                    std::uint8_t boundary_pinned = 0) noexcept {
        if (match_count >= kMaxInlineMatches)
            return false;
        matches[match_count++] = QueryResultMatch{node_id,
                                                  tenant_id,
                                                  fiber_id,
                                                  mutation_id_at_capture,
                                                  generation,
                                                  wrap_epoch,
                                                  cow_epoch_at_capture,
                                                  boundary_pinned,
                                                  /*reserved=*/0};
        return true;
    }

    // Issue #3103: capture-time stamping overload. Builds a schema-2
    // QueryResultMatch with the full StableNodeRef provenance. Production
    // Agent loops use this from :as-query-result / :query-result #t finish
    // paths. Soft / single-tenant bare path keeps the 2-arg push_match.
    // Issue #3231: finish path gated on production_defaults_active() must
    // not accept schema-1 (layout-only) after this overload.
    bool push_match_full(std::uint32_t node_id, std::uint16_t generation, std::uint16_t wrap_epoch,
                         std::uint16_t cow_epoch_at_capture, std::uint32_t tenant_id,
                         std::uint32_t fiber_id, std::uint32_t mutation_id_at_capture,
                         std::uint8_t boundary_pinned) noexcept {
        return push_match(node_id, generation, wrap_epoch, cow_epoch_at_capture, tenant_id,
                          fiber_id, mutation_id_at_capture, boundary_pinned);
    }
};

// Process-wide metrics (additive; multi-fiber relaxed).
inline std::atomic<std::uint64_t>& g_query_result_created_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_query_result_fresh_hits_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_query_result_stale_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint32_t>& g_query_result_wired() noexcept {
    static std::atomic<std::uint32_t> v{1};
    return v;
}
inline constexpr int kQueryResultIssue = 2933;
inline constexpr int kQueryResultFullProvenanceIssue = 3103;
// Issue #3231: production :as-query-result must not export schema-1.
inline constexpr int kQueryResultLayoutOnlyRejectIssue = 3231;
inline constexpr const char* kQueryResultLayoutOnlyErrorKind = "query-result-layout-only";
inline constexpr std::uint8_t kQueryResultMatchSchema2 = 1;

// Issue #3103: full-provenance path observability (additive; production/Full
// keeps the schema-1 Soft counters untouched). Bumped at capture-time when
// :as-query-result / :query-result #t stamps a schema-2 match, and at
// is_fresh_with_refs validation time on the full-provenance path.
inline std::atomic<std::uint64_t>& g_query_result_full_provenance_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_query_result_full_provenance_fresh_hits_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_query_result_full_provenance_stale_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_query_result_full_provenance_tenant_mismatch_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_query_result_full_provenance_fiber_mismatch_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_query_result_full_provenance_cow_mismatch_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint32_t>& g_query_result_full_provenance_wired() noexcept {
    static std::atomic<std::uint32_t> v{1};
    return v;
}

inline void note_query_result_full_provenance() noexcept {
    g_query_result_full_provenance_total().fetch_add(1, std::memory_order_relaxed);
}
inline void note_query_result_full_provenance_fresh_hit() noexcept {
    g_query_result_full_provenance_fresh_hits_total().fetch_add(1, std::memory_order_relaxed);
}
inline void note_query_result_full_provenance_stale() noexcept {
    g_query_result_full_provenance_stale_total().fetch_add(1, std::memory_order_relaxed);
}
inline void note_query_result_full_provenance_tenant_mismatch() noexcept {
    g_query_result_full_provenance_tenant_mismatch_total().fetch_add(1, std::memory_order_relaxed);
}
inline void note_query_result_full_provenance_fiber_mismatch() noexcept {
    g_query_result_full_provenance_fiber_mismatch_total().fetch_add(1, std::memory_order_relaxed);
}
inline void note_query_result_full_provenance_cow_mismatch() noexcept {
    g_query_result_full_provenance_cow_mismatch_total().fetch_add(1, std::memory_order_relaxed);
}

inline void reset_query_result_full_provenance_for_test() noexcept {
    g_query_result_full_provenance_total().store(0, std::memory_order_relaxed);
    g_query_result_full_provenance_fresh_hits_total().store(0, std::memory_order_relaxed);
    g_query_result_full_provenance_stale_total().store(0, std::memory_order_relaxed);
    g_query_result_full_provenance_tenant_mismatch_total().store(0, std::memory_order_relaxed);
    g_query_result_full_provenance_fiber_mismatch_total().store(0, std::memory_order_relaxed);
    g_query_result_full_provenance_cow_mismatch_total().store(0, std::memory_order_relaxed);
}

inline void note_query_result_created() noexcept {
    g_query_result_created_total().fetch_add(1, std::memory_order_relaxed);
}
inline void note_query_result_fresh_hit() noexcept {
    g_query_result_fresh_hits_total().fetch_add(1, std::memory_order_relaxed);
}
inline void note_query_result_stale() noexcept {
    g_query_result_stale_total().fetch_add(1, std::memory_order_relaxed);
}

// Issue #3103: full-provenance freshness validator. Returns the reason
// the QueryResult is not fresh (or Fresh). Production Agent loops opt
// into the schema-2 path via :as-query-result / :query-result #t and
// rely on this to refuse silent rebinds under multi-tenant / COW /
// wrap / concurrent fiber contention. The header stays layout-only —
// the implementation is in evaluator_primitives_query_workspace.cpp
// which already includes both this header + FlatAST.
//
// tenant_id / fiber_id are passed in (default 0 = skip tenant/fiber
// check, Soft path) because they live on the Evaluator, not FlatAST.
// Pass the live Evaluator::capability_tenant_id() + the live fiber_id
// under production defaults to enforce multi-tenant / concurrent-fiber
// isolation; pass 0 to opt into the Soft single-tenant path.
enum class QueryResultFreshness {
    Fresh = 0,
    StaleByEpoch = 1,         // mutation_epoch + flat generation advanced
    InvalidTenant = 2,        // tenant_id mismatch (multi-tenant isolation)
    InvalidFiber = 3,         // fiber_id mismatch (concurrent fiber steal)
    InvalidCowLayer = 4,      // cow_epoch_at_capture vs live cow_epoch
    InvalidMutation = 5,      // mutation_id_at_capture vs live mutation_epoch
    SoftOnlyNoProvenance = 6, // schema-1 layout-only matches (no ref stamp)
};

// `flat` is the live FlatAST (void* so this layout-only header does not
// forward-declare aura::ast::FlatAST — that collides with the module
// export in TUs that `import aura.core.ast` and include this header).
[[nodiscard]] QueryResultFreshness
query_result_is_fresh_with_refs(const QueryResult& qr, const void* flat,
                                std::uint64_t current_tenant_id = 0,
                                std::uint64_t current_fiber_id = 0) noexcept;

// only if epoch-fresh (Agents always get accurate is_fresh).
[[nodiscard]] inline bool query_result_check_fresh(const QueryResult& qr,
                                                   std::uint64_t flat_generation) noexcept {
    // Issue #3041: production restamp-budget exceed forces held results stale
    // immediately (do not wait for is_valid / refresh_if_stale).
    if (g_query_epoch_forced_stale().load(std::memory_order_acquire) != 0) {
        note_query_result_stale();
        return false;
    }
    if (qr.is_fresh_live(flat_generation)) {
        note_query_result_fresh_hit();
        return true;
    }
    note_query_result_stale();
    return false;
}

inline void reset_query_result_metrics_for_test() noexcept {
    g_query_result_created_total().store(0, std::memory_order_relaxed);
    g_query_result_fresh_hits_total().store(0, std::memory_order_relaxed);
    g_query_result_stale_total().store(0, std::memory_order_relaxed);
}

// Extend #2192 test reset so #2933 metrics clear together when tests reset.
inline void reset_query_epoch_and_result_metrics_for_test() noexcept {
    reset_query_epoch_metrics_for_test();
    reset_query_result_metrics_for_test();
}

} // namespace aura::core

#endif // AURA_CORE_WORKSPACE_EPOCH_HH
