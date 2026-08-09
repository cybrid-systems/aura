// ownership_rebind.h — Issue #2695 unified OwnershipEnv rebind API + Issue #2708
// real per-root validate walk.
//
// Post-densify (live_compact Moving) + fiber steal + explicit Agent
// mutate:rebind all need to rebind OwnershipEnv after a remapped-root
// event so Owned never lingers pointing at remapped storage until the next
// full validate. Before #2695 the rebind work was scattered across
// Phase-5 densify exit + steal resume + post_mutate_enforce_all + the
// Agent rebind primitive — no single entry Agents / EDSL could call, so
// residual windows could remain where Owned pointed at remapped storage
// until the next full validate.
//
// #2695 first ship: unified entry + per-reason counters + Soft/Production
// routing. Real per-root walk through OwnershipEnv::validate_ownership was
// deferred (per-root env walk lives in the type_checker module, pure
// header cannot name the module type without GCC 16 ambiguity).
//
// #2708 second ship: real per-root walk via a module-side bridge hook
// (aura_ownership_rebind_walk_root in ownership_rebind.cpp — same TU as
// the unified entry to avoid header ↔ module cycle). The walk:
//   - AC3 empty-span short-circuit preserved (zero-cost when no remap).
//   - For each remapped root: bumps g_ownership_rebind_validate_walk_total
//     and g_ownership_rebind_*_total per-reason.
//   - Production / Full path on test-injected mismatch: returns false so
//     the caller triggers force_linear_rollback per #2563 contract.
//   - Soft path: observe only — counter bumps but function returns true.
//
// Test injection: g_ownership_rebind_test_injected_root holds a single
// sentinel NodeId; inject_ownership_rebind_mismatch_for_test() seeds it;
// when any remapped_root matches, the walk routes through the mismatch
// branch. Pure test surface — production builds never call the injection
// hooks, so there is no observable behavior change in production unless a
// caller passes a non-empty span AND has injected a mismatch root (test
// only).
//
// Usage:
//   std::vector<std::uint32_t> roots{...};  // NodeId == uint32_t
//   if (!aura::compiler::ownership_rebind_after_remap(
//           std::span<const OwnershipRebindNodeId>(roots.data(), roots.size()),
//           RemapReason::Densify)) {
//       // production force rollback
//   }
//
// OwnershipEnv is intentionally NOT a parameter on the surface: the env
// class lives in the type_checker module; a header-level `class
// OwnershipEnv` forward-decl collides with the module export under GCC 16
// partitions (ambiguous OwnershipEnv). #2708 keeps this constraint — the
// walk is a TU-local extern "C" hook in ownership_rebind.cpp.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace aura::compiler {

// Issue #2695: why rebind is being invoked. Counters + observability
// surface this enum so Agents can distinguish densify-driven vs
// steal-driven vs explicit-Agent-driven rebinds in dashboards.
enum class RemapReason : std::uint8_t {
    Densify = 0,
    Steal = 1,
    ExplicitAgent = 2,
};

// Issue #2695: original unified entry + counter surface.
inline constexpr int kOwnershipRebindIssue = 2695;
// Issue #2708: real per-root validate walk + test-injection hooks.
inline constexpr int kOwnershipRebindWalkIssue = 2708;

// NodeId is `using NodeId = std::uint32_t` in aura.core (mutation.ixx).
// This pure header must NOT forward-declare `struct NodeId` — an incomplete
// class type makes `const NodeId*` non-contiguous under GCC 16 concepts
// (`++` on incomplete pointer is ill-formed), so every std::span ctor is
// SFINAE'd out (asan-build / CI). Use the POD alias instead; same width
// as the canonical NodeId, no module import, no redefinition vs import
// aura.core.ast.
using OwnershipRebindNodeId = std::uint32_t;

// Lifetime counters. File-scope atomics so light binaries (without
// per-CompilerMetrics wiring) still surface observability in queries
// (#2695 mirrors the #2693 / #2694 file-scope counter pattern).
inline std::atomic<std::uint64_t> g_ownership_rebind_total{0};
inline std::atomic<std::uint64_t> g_ownership_rebind_fail_total{0};
inline std::atomic<std::uint32_t> g_ownership_rebind_wired{1};
// Per-reason counters (header-visible so query surfaces can read them
// without a separate .cpp-only export).
inline std::atomic<std::uint64_t> g_ownership_rebind_densify_total{0};
inline std::atomic<std::uint64_t> g_ownership_rebind_steal_total{0};
inline std::atomic<std::uint64_t> g_ownership_rebind_explicit_agent_total{0};
inline std::atomic<std::uint64_t> g_ownership_rebind_densify_fail_total{0};
inline std::atomic<std::uint64_t> g_ownership_rebind_steal_fail_total{0};
inline std::atomic<std::uint64_t> g_ownership_rebind_explicit_agent_fail_total{0};
// Issue #2708: lifetime walks that actually entered the per-root loop
// (i.e. remapped_roots was non-empty). Lets dashboards distinguish
// zero-cost short-circuits (AC3) from real rebinds that walked the span.
inline std::atomic<std::uint64_t> g_ownership_rebind_validate_walk_total{0};
// Issue #2723: lifetime walks that received a non-empty span from
// production call sites (densify Phase-5 + steal resume). Lets dashboards
// distinguish "call site still passes empty" (zero-cost short-circuit —
// pre-#2723) from "call site wired non-empty" (real rebind under
// densify/steal). AC4 (single source of truth — densify + steal share
// the same helper) + AC5 (additive observability only).
inline std::atomic<std::uint64_t> g_ownership_rebind_nonempty_span_total{0};
// Issue #2742: helper fell back to dirty-pin / densify-affected NodeIds
// because linear_roots() was empty. Distinguishes linear primary path
// from dirty/pin secondary path under densify × steal production load.
inline std::atomic<std::uint64_t> g_ownership_rebind_dirty_fallback_total{0};
inline constexpr int kOwnershipRebindDirtyFallbackIssue = 2742;

// Issue #2854: same-transaction order — the boundary needs a structured
// rebind outcome to stamp TypeLinearCommitProof after rebind + scan (no
// success proof may outlive a failed rebind on the same exit). Mirrors
// the #2372 production-lock file-scope pattern (atomic state for
// cross-call-site visibility from save_hygiene_checkpoint / steal resume).
//
// Field semantics:
//   - rebind_ok: true if the per-root walk passed (or span was empty →
//     zero-cost short-circuit preserved, AC4 #2723). false on
//     production / Full mismatch (caller triggers force_linear_rollback
//     per #2563 contract).
//   - root_count: number of remapped roots in the span (== post-remap
//     collect for the proof's linear_root_count).
//   - had_mismatch: true if Soft mismatch was observed (counter bumps
//     but function still returns true — distinct from rebind_ok which
//     is the function return).
//   - reason: which RemapReason the last call used.
//   - had_rebind: true if rebind was attempted (vs empty span quiet
//     short-circuit). Lets Phase-5 stamp distinguish "no rebind needed"
//     (Quiet, AC4) from "rebind ran + ok" (Stamped).
struct OwnershipRebindReport {
    bool rebind_ok;
    std::size_t root_count;
    bool had_mismatch;
    RemapReason reason;
    bool had_rebind;
};

// File-scope atomics — the per-root walk sets them at function exit;
// Phase-5 densify block + steal resume + save_hygiene_checkpoint read
// them. Reset on every call (so a stale value from a previous exit
// never leaks into a quiet path).
inline std::atomic<std::uint8_t> g_ownership_rebind_last_ok{1}; // default rebind_ok=true
inline std::atomic<std::size_t> g_ownership_rebind_last_root_count{0};
inline std::atomic<std::uint8_t> g_ownership_rebind_last_had_mismatch{0};
inline std::atomic<std::uint8_t> g_ownership_rebind_last_reason{0}; // RemapReason::Densify=0
inline std::atomic<std::uint8_t> g_ownership_rebind_last_had_rebind{0};
inline constexpr int kOwnershipRebindSameTransactionOrderIssue = 2854;

[[nodiscard]] inline OwnershipRebindReport last_ownership_rebind_report_v_read() noexcept {
    OwnershipRebindReport r{};
    r.rebind_ok = g_ownership_rebind_last_ok.load(std::memory_order_acquire) != 0;
    r.root_count = g_ownership_rebind_last_root_count.load(std::memory_order_acquire);
    r.had_mismatch = g_ownership_rebind_last_had_mismatch.load(std::memory_order_acquire) != 0;
    r.reason =
        static_cast<RemapReason>(g_ownership_rebind_last_reason.load(std::memory_order_acquire));
    r.had_rebind = g_ownership_rebind_last_had_rebind.load(std::memory_order_acquire) != 0;
    return r;
}

// Test reset — clears the last-rebind file-scope atomics to the quiet
// default so back-to-back tests don't observe stale state.
inline void clear_last_ownership_rebind_report_for_test() noexcept {
    g_ownership_rebind_last_ok.store(1, std::memory_order_release);
    g_ownership_rebind_last_root_count.store(0, std::memory_order_release);
    g_ownership_rebind_last_had_mismatch.store(0, std::memory_order_release);
    g_ownership_rebind_last_reason.store(0, std::memory_order_release);
    g_ownership_rebind_last_had_rebind.store(0, std::memory_order_release);
}
// Test-injected mismatch sentinel. ~0u is the "no mismatch" sentinel —
// chosen because NodeId 0xFFFFFFFF is reserved (NULL_NODE / out-of-range).
// Atomic so a concurrent test injector + the walk TU don't race on plain
// load/store (aura_test_objects are TSAN-instrumented).
inline std::atomic<std::uint32_t> g_ownership_rebind_test_injected_root{~0u};
// Issue #2742 test inject: densify-affected NodeIds when no linear roots.
// Size capped (kOwnershipRebindDirtyInjectCap). n==0 means inactive.
inline constexpr std::size_t kOwnershipRebindDirtyInjectCap = 16;
inline std::atomic<std::uint32_t> g_ownership_rebind_dirty_inject_n{0};
inline std::array<OwnershipRebindNodeId, kOwnershipRebindDirtyInjectCap>
    g_ownership_rebind_dirty_inject_ids{};

// Read-side accessors (C ABI stable — surfaces in `query:ownership-rebind-stats`).
[[nodiscard]] inline std::uint64_t ownership_rebind_total_v_read() noexcept {
    return g_ownership_rebind_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t ownership_rebind_fail_total_v_read() noexcept {
    return g_ownership_rebind_fail_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t ownership_rebind_wired_v_read() noexcept {
    return g_ownership_rebind_wired.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t ownership_rebind_densify_total_v_read() noexcept {
    return g_ownership_rebind_densify_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t ownership_rebind_steal_total_v_read() noexcept {
    return g_ownership_rebind_steal_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t ownership_rebind_explicit_agent_total_v_read() noexcept {
    return g_ownership_rebind_explicit_agent_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t ownership_rebind_densify_fail_total_v_read() noexcept {
    return g_ownership_rebind_densify_fail_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t ownership_rebind_steal_fail_total_v_read() noexcept {
    return g_ownership_rebind_steal_fail_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t ownership_rebind_explicit_agent_fail_total_v_read() noexcept {
    return g_ownership_rebind_explicit_agent_fail_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t ownership_rebind_validate_walk_total_v_read() noexcept {
    return g_ownership_rebind_validate_walk_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t ownership_rebind_nonempty_span_total_v_read() noexcept {
    return g_ownership_rebind_nonempty_span_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t ownership_rebind_dirty_fallback_total_v_read() noexcept {
    return g_ownership_rebind_dirty_fallback_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t ownership_rebind_test_injected_root_v_read() noexcept {
    return g_ownership_rebind_test_injected_root.load(std::memory_order_relaxed);
}

// Test reset. Hermetic for tests/serve/test_steal_densify_linear_type_hard_and.cpp
// extension per #81967 — does NOT touch real env state.
inline void clear_ownership_rebind_for_test() noexcept {
    g_ownership_rebind_total.store(0, std::memory_order_relaxed);
    g_ownership_rebind_fail_total.store(0, std::memory_order_relaxed);
    g_ownership_rebind_densify_total.store(0, std::memory_order_relaxed);
    g_ownership_rebind_steal_total.store(0, std::memory_order_relaxed);
    g_ownership_rebind_explicit_agent_total.store(0, std::memory_order_relaxed);
    g_ownership_rebind_densify_fail_total.store(0, std::memory_order_relaxed);
    g_ownership_rebind_steal_fail_total.store(0, std::memory_order_relaxed);
    g_ownership_rebind_explicit_agent_fail_total.store(0, std::memory_order_relaxed);
    g_ownership_rebind_validate_walk_total.store(0, std::memory_order_relaxed);
    g_ownership_rebind_nonempty_span_total.store(0, std::memory_order_relaxed);
    g_ownership_rebind_dirty_fallback_total.store(0, std::memory_order_relaxed);
    g_ownership_rebind_test_injected_root.store(~0u, std::memory_order_relaxed);
    g_ownership_rebind_dirty_inject_n.store(0, std::memory_order_relaxed);
    for (auto& id : g_ownership_rebind_dirty_inject_ids)
        id = 0;
}

// Issue #2742: seed densify-affected / dirty-pin NodeIds for the helper
// fallback path (when linear_roots() is empty). Production never calls this.
inline void
inject_ownership_rebind_dirty_roots_for_test(std::span<const OwnershipRebindNodeId> ids) noexcept {
    const auto n = std::min(ids.size(), kOwnershipRebindDirtyInjectCap);
    for (std::size_t i = 0; i < n; ++i)
        g_ownership_rebind_dirty_inject_ids[i] = ids[i];
    g_ownership_rebind_dirty_inject_n.store(static_cast<std::uint32_t>(n),
                                            std::memory_order_relaxed);
}
inline void clear_ownership_rebind_dirty_roots_for_test() noexcept {
    g_ownership_rebind_dirty_inject_n.store(0, std::memory_order_relaxed);
}

// Test injection hook — Issue #2708 AC1/AC2 production/soft mismatch.
// Seeds the sentinel NodeId that the walk compares each remapped_root
// against. ~0u is the "no mismatch" default (matches the atomic's init).
// Production builds should never call this — the test injects via the
// test-only reset path so production behavior is unchanged.
inline void inject_ownership_rebind_mismatch_for_test(OwnershipRebindNodeId root) noexcept {
    g_ownership_rebind_test_injected_root.store(root, std::memory_order_relaxed);
}
// Test-only clear — alias of the broader reset for symmetry.
inline void clear_ownership_rebind_mismatch_for_test() noexcept {
    g_ownership_rebind_test_injected_root.store(~0u, std::memory_order_relaxed);
}

// The unified entry. Returns true on rebind success / zero-cost short-circuit
// (empty remapped_roots). Returns false on production mismatch (caller
// triggers force_linear_rollback per #2563). Soft mismatch is observed
// only — the counter bumps but the function still returns true.
// remapped_roots element type is OwnershipRebindNodeId (== NodeId / uint32_t).
bool ownership_rebind_after_remap(std::span<const OwnershipRebindNodeId> remapped_roots,
                                  RemapReason why) noexcept;

// Issue #2723 / #2742: non-empty span collector for densify Phase-5 + steal
// resume. Single source of truth (AC4): both call sites route through this
// helper. Primary: linear_roots(). Fallback when empty (#2742): test-inject
// densify-affected NodeIds + live LifetimePin registry (dirty-pin). Returns
// std::span over a thread-local scratch buffer (AC3 zero-cost when empty).
// Lifetime: until the next call from the same thread.
std::span<const OwnershipRebindNodeId> collect_linear_or_dirty_roots_for_rebind() noexcept;
// Issue #2758: size-only wrapper for TypeLinearCommitProof stamp (avoids
// typed_mutation_audit.h including this header; quiet path empty → 0).
[[nodiscard]] std::size_t linear_or_dirty_roots_count_for_rebind() noexcept;

// C ABI overload for tests / FFI bridges that pass raw ptr + size.
// std::span(ptr, count) is well-formed once the element type is complete
// POD (uint32_t) — see OwnershipRebindNodeId note above.
[[nodiscard]] inline bool ownership_rebind_after_remap_c(const OwnershipRebindNodeId* roots,
                                                         std::size_t n, RemapReason why) noexcept {
    return ownership_rebind_after_remap(std::span<const OwnershipRebindNodeId>(roots, n), why);
}

} // namespace aura::compiler