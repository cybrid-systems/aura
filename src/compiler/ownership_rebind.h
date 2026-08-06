// ownership_rebind.h — Issue #2695 unified OwnershipEnv rebind API.
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
// This header is the unified entry. Pure header (no module cycle on
// type_checker / arena / fiber). Implementation in ownership_rebind.cpp.
//
// Soft: observe mismatch (count via g_ownership_rebind_fail_total) and
// return true — caller continues; Soft paths under #2545 / #2673 already
// have downstream validate that catches residual drift.
// production / Full: mismatch returns false → caller triggers
// force_linear_rollback per #2563 contract.
//
// Usage:
//   std::vector<std::uint32_t> roots{...};  // NodeId == uint32_t
//   if (!aura::compiler::ownership_rebind_after_remap(
//           std::span<const OwnershipRebindNodeId>(roots.data(), roots.size()),
//           RemapReason::Densify)) {
//       // production force rollback
//   }
//
// OwnershipEnv is intentionally NOT a parameter on the first-ship surface:
// the env class lives in the type_checker module; a header-level
// `class OwnershipEnv` forward-decl collides with the module export under
// GCC 16 partitions (ambiguous OwnershipEnv). Per-root env walk is a
// follow-up that will take a module-side wrapper, not this pure header.

#pragma once

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

inline constexpr int kOwnershipRebindIssue = 2695;

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

// Test reset. Hermetic for tests/serve/test_steal_densify_linear_type_hard_and.cpp
// extension per #81967 — does NOT touch real env state.
inline void clear_ownership_rebind_for_test() noexcept {
    g_ownership_rebind_total.store(0, std::memory_order_relaxed);
    g_ownership_rebind_fail_total.store(0, std::memory_order_relaxed);
}

// The unified entry. Returns true on rebind success / zero-cost short-circuit
// (empty remapped_roots). Returns false on production mismatch (caller
// triggers force_linear_rollback per #2563). Soft mismatch is observed
// only — the counter bumps but the function still returns true.
// remapped_roots element type is OwnershipRebindNodeId (== NodeId / uint32_t).
bool ownership_rebind_after_remap(std::span<const OwnershipRebindNodeId> remapped_roots,
                                  RemapReason why) noexcept;

// C ABI overload for tests / FFI bridges that pass raw ptr + size.
// std::span(ptr, count) is well-formed once the element type is complete
// POD (uint32_t) — see OwnershipRebindNodeId note above.
[[nodiscard]] inline bool ownership_rebind_after_remap_c(const OwnershipRebindNodeId* roots,
                                                         std::size_t n, RemapReason why) noexcept {
    return ownership_rebind_after_remap(std::span<const OwnershipRebindNodeId>(roots, n), why);
}

} // namespace aura::compiler