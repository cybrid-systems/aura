// moving_densify_health.hh — Issue #2619: Agent-visible Moving densify health.
//
// Pairs with #2596 (AURA_MOVING_UNTRACKED=hard under production). Runtime
// hard-abort lives in arena.ixx; this surface answers:
//   "is Moving densify safe to rely on for the next mutate batch?"
//
// Zero cost when Moving off / no densify (last-window defaults = healthy;
// publish is atomic stores only after densify work). Soft/sandbox remains
// observe-only unless production-hard (#2596 pref) is active.
//
// Soft throttle: agent_throttle_for_moving_densify mirrors mailbox
// starvation throttle (#2551) so orch can refuse new mutate without
// waiting for Phase 5 hard abort.

#ifndef AURA_CORE_MOVING_DENSIFY_HEALTH_HH
#define AURA_CORE_MOVING_DENSIFY_HEALTH_HH

#include "core/arena_auto_policy_stats.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace aura::core::moving_densify_health {

inline constexpr int kMovingDensifyHealthIssue = 2619;
// Lineage: #2596 production hard, #2495 untracked incomplete-remap fail-closed.
inline constexpr int kMovingUntrackedHardIssue = 2596;
inline constexpr int kMovingDensifyFailClosedIssue = 2495;

// ── Last densify window (Soft / no densify → healthy defaults) ──
inline std::atomic<std::uint64_t> g_last_objects_moved{0};
inline std::atomic<std::uint64_t> g_last_untracked_kept{0};
inline std::atomic<std::uint8_t> g_last_pin_contract_held{1};
inline std::atomic<std::uint8_t> g_last_moving_incomplete_remap{0};
inline std::atomic<std::uint64_t> g_last_root_remap_fail_total{0};
inline std::atomic<std::uint8_t> g_last_had_moving_densify{0};
inline std::atomic<std::uint64_t> g_last_window_seq{0};

// Soft orch throttle (1 = refuse new mutate until healthy densify).
inline std::atomic<std::uint8_t> g_agent_throttle_for_moving_densify{0};
inline std::atomic<std::uint64_t> g_agent_throttle_moving_densify_set_total{0};
inline std::atomic<std::uint64_t> g_agent_throttle_moving_densify_clear_total{0};

// Force-reason codes for Agents (stable ints).
inline constexpr std::int64_t kForceNone = 0;
inline constexpr std::int64_t kForceUntrackedIncomplete = 1;
inline constexpr std::int64_t kForcePin = 2;
inline constexpr std::int64_t kForceRootRemap = 3;

struct MovingDensifyHealthSnapshot {
    bool production_hard_active = false;
    bool moving_compact_enabled = false;
    bool had_moving_densify = false;
    bool pin_contract_held = true;
    bool moving_incomplete_remap = false;
    std::uint64_t objects_moved = 0;
    std::uint64_t untracked_kept = 0;
    std::uint64_t root_remap_fail_total = 0;
    std::uint64_t untracked_external_roots_total = 0;
    std::uint64_t objects_moved_total = 0;
    std::uint64_t moving_blocked_precondition_total = 0;
    std::uint64_t window_seq = 0;
    bool would_allow_mutate = true;
    std::int64_t force_reason_code = kForceNone;
    bool agent_throttle = false;
};

// Production-hard active when #2596 pref is hard (1). Soft/sandbox
// leaves pref at -1 (observe-only) unless operator sets env=hard.
[[nodiscard]] inline bool production_hard_active() noexcept {
    return aura::ast::g_moving_untracked_hard_abort_pref.load(std::memory_order_relaxed) > 0;
}

// Env / pref for Moving compact on (arena.ixx also has moving_compact_enabled;
// we duplicate a soft probe via env to avoid module import from this header).
[[nodiscard]] inline bool moving_compact_enabled_probe() noexcept {
    const char* e = std::getenv("AURA_ARENA_MOVING_COMPACT");
    if (e != nullptr) {
        if (e[0] == '0')
            return false;
        if (e[0] == '1')
            return true;
    }
    return true; // #2256 production default ON
}

// Window is safe for next mutate when: no densify, or densify held pin +
// no incomplete remap + no untracked kept + no root-remap fails.
[[nodiscard]] inline bool window_would_allow_mutate(bool had_densify, bool pin_held,
                                                    bool incomplete, std::uint64_t untracked_kept,
                                                    std::uint64_t root_fail) noexcept {
    if (!had_densify)
        return true; // vacuous healthy (AC4)
    return pin_held && !incomplete && untracked_kept == 0 && root_fail == 0;
}

// Issue #2682: single unified Moving success predicate (AC1-AC4).
//   success_moving ⇔
//     !moving_blocked_precondition
//     ∧ pin_contract_held
//     ∧ root_remap_stable_ref_fail_total == 0
//     ∧ root_remap_closure_capture_fail_total == 0
//     ∧ untracked_kept_count == 0   // when objects_moved > 0
// Used by Phase-5 outermost exit, AdaptiveCompactResult consumers, and
// Agent health surface. Replaces the scattered local-variable checks
// that previously lived inline in evaluator_mutation_boundary.cpp.
[[nodiscard]] inline bool compute_moving_unified_success(
    bool moving_blocked_precondition,
    bool pin_contract_held,
    std::uint64_t root_remap_stable_ref_fail_total,
    std::uint64_t root_remap_closure_capture_fail_total,
    std::uint64_t objects_moved,
    std::uint64_t untracked_kept_count) noexcept {
    if (moving_blocked_precondition)
        return false;
    if (!pin_contract_held)
        return false;
    if (root_remap_stable_ref_fail_total > 0)
        return false;
    if (root_remap_closure_capture_fail_total > 0)
        return false;
    if (objects_moved > 0 && untracked_kept_count > 0)
        return false;
    return true;
}

[[nodiscard]] inline std::int64_t compute_force_reason(bool pin_held, bool incomplete,
                                                       std::uint64_t untracked_kept,
                                                       std::uint64_t root_fail) noexcept {
    if (incomplete || untracked_kept > 0)
        return kForceUntrackedIncomplete;
    if (!pin_held)
        return kForcePin;
    if (root_fail > 0)
        return kForceRootRemap;
    return kForceNone;
}

// Publish last densify window + soft throttle under production-hard.
// Called from Phase 5 / compact_all_moving_pinned / test inject.
// Soft/sandbox: still publish last-window (observe) but only set throttle
// when production_hard_active (AC3).
inline void publish_last_moving_densify_window(bool had_moving_densify, bool pin_contract_held,
                                               bool moving_incomplete_remap,
                                               std::uint64_t objects_moved,
                                               std::uint64_t untracked_kept,
                                               std::uint64_t root_remap_fail_total) noexcept {
    g_last_had_moving_densify.store(had_moving_densify ? 1 : 0, std::memory_order_relaxed);
    g_last_pin_contract_held.store(pin_contract_held ? 1 : 0, std::memory_order_relaxed);
    g_last_moving_incomplete_remap.store(moving_incomplete_remap ? 1 : 0,
                                         std::memory_order_relaxed);
    g_last_objects_moved.store(objects_moved, std::memory_order_relaxed);
    g_last_untracked_kept.store(untracked_kept, std::memory_order_relaxed);
    g_last_root_remap_fail_total.store(root_remap_fail_total, std::memory_order_relaxed);
    g_last_window_seq.fetch_add(1, std::memory_order_relaxed);

    const bool allow =
        window_would_allow_mutate(had_moving_densify, pin_contract_held, moving_incomplete_remap,
                                  untracked_kept, root_remap_fail_total);
    // Soft throttle only under production-hard (#2596). Soft/sandbox observe-only.
    if (production_hard_active() && had_moving_densify && !allow) {
        const auto prev =
            g_agent_throttle_for_moving_densify.exchange(1, std::memory_order_relaxed);
        if (prev == 0)
            g_agent_throttle_moving_densify_set_total.fetch_add(1, std::memory_order_relaxed);
    } else if (allow) {
        const auto prev =
            g_agent_throttle_for_moving_densify.exchange(0, std::memory_order_relaxed);
        if (prev != 0)
            g_agent_throttle_moving_densify_clear_total.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void clear_agent_throttle_for_moving_densify() noexcept {
    const auto prev = g_agent_throttle_for_moving_densify.exchange(0, std::memory_order_relaxed);
    if (prev != 0)
        g_agent_throttle_moving_densify_clear_total.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline bool agent_throttle_for_moving_densify() noexcept {
    return g_agent_throttle_for_moving_densify.load(std::memory_order_relaxed) != 0;
}

// Test-only: reset last window to vacuous healthy.
inline void reset_moving_densify_health_for_test() noexcept {
    g_last_objects_moved.store(0, std::memory_order_relaxed);
    g_last_untracked_kept.store(0, std::memory_order_relaxed);
    g_last_pin_contract_held.store(1, std::memory_order_relaxed);
    g_last_moving_incomplete_remap.store(0, std::memory_order_relaxed);
    g_last_root_remap_fail_total.store(0, std::memory_order_relaxed);
    g_last_had_moving_densify.store(0, std::memory_order_relaxed);
    g_last_window_seq.store(0, std::memory_order_relaxed);
    g_agent_throttle_for_moving_densify.store(0, std::memory_order_relaxed);
}

// Pure snapshot for query / orch (no side effects).
// Process totals are loaded via function pointers / extern atomics set by
// callers that include arena.ixx — or pass 0 and let query fill from arena.
struct ProcessTotals {
    std::uint64_t untracked_external_roots_total = 0;
    std::uint64_t objects_moved_total = 0;
    std::uint64_t moving_blocked_precondition_total = 0;
};

[[nodiscard]] inline MovingDensifyHealthSnapshot
snapshot(const ProcessTotals& totals = {}) noexcept {
    MovingDensifyHealthSnapshot s;
    s.production_hard_active = production_hard_active();
    s.moving_compact_enabled = moving_compact_enabled_probe();
    s.had_moving_densify = g_last_had_moving_densify.load(std::memory_order_relaxed) != 0;
    s.pin_contract_held = g_last_pin_contract_held.load(std::memory_order_relaxed) != 0;
    s.moving_incomplete_remap = g_last_moving_incomplete_remap.load(std::memory_order_relaxed) != 0;
    s.objects_moved = g_last_objects_moved.load(std::memory_order_relaxed);
    s.untracked_kept = g_last_untracked_kept.load(std::memory_order_relaxed);
    s.root_remap_fail_total = g_last_root_remap_fail_total.load(std::memory_order_relaxed);
    s.untracked_external_roots_total = totals.untracked_external_roots_total;
    s.objects_moved_total = totals.objects_moved_total;
    s.moving_blocked_precondition_total = totals.moving_blocked_precondition_total;
    s.window_seq = g_last_window_seq.load(std::memory_order_relaxed);
    s.would_allow_mutate = window_would_allow_mutate(s.had_moving_densify, s.pin_contract_held,
                                                     s.moving_incomplete_remap, s.untracked_kept,
                                                     s.root_remap_fail_total);
    s.force_reason_code = s.would_allow_mutate
                              ? kForceNone
                              : compute_force_reason(s.pin_contract_held, s.moving_incomplete_remap,
                                                     s.untracked_kept, s.root_remap_fail_total);
    s.agent_throttle = agent_throttle_for_moving_densify();
    return s;
}

inline std::atomic<std::uint64_t> g_moving_densify_health_wired{1};

[[nodiscard]] inline std::uint64_t moving_densify_health_wired() noexcept {
    return g_moving_densify_health_wired.load(std::memory_order_acquire);
}

} // namespace aura::core::moving_densify_health

#endif // AURA_CORE_MOVING_DENSIFY_HEALTH_HH
