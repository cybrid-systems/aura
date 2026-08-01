// mutation_hold_budget.h — Issue #2313: hold-budget threshold accessor.
// Shared header so MutationBoundaryGuard dtor + query:mutation-boundary-hold-stats
// share one env-cached source of truth (AURA_MUTATION_HOLD_BUDGET_US).
// Issue #2517: process-wide live longest outermost hold probe (fiber_id +
// start_ns) for Agent self-degrade during long mutate.

#ifndef AURA_COMPILER_MUTATION_HOLD_BUDGET_H
#define AURA_COMPILER_MUTATION_HOLD_BUDGET_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace aura::compiler {

// Default 100_000 µs (100 ms). Lazy-init from AURA_MUTATION_HOLD_BUDGET_US.
// C-style digit parse (no exceptions). Cached once per process.
[[nodiscard]] inline std::uint64_t mutation_hold_budget_us() noexcept {
    static const std::uint64_t cached = []() noexcept -> std::uint64_t {
        const char* e = std::getenv("AURA_MUTATION_HOLD_BUDGET_US");
        if (e == nullptr || e[0] == '\0')
            return 100000ULL;
        std::uint64_t v = 0;
        for (const char* p = e; *p >= '0' && *p <= '9'; ++p) {
            v = v * 10 + static_cast<std::uint64_t>(*p - '0');
        }
        return v > 0 ? v : 100000ULL;
    }();
    return cached;
}

// Issue #2349: production hold SLO circuit-breaker (default force-fail path).
// Distinct from #2313 signal-only budget and #2199 opt-in STRICT hard-timeout.
//
// ── Decision table (Soft / Production / Disabled) ──
// | Mode       | Env select                                   | hold_us > SLO action              |
// | Soft       | AURA_SANDBOX=off OR AURA_MUTATION_HOLD_SLO_SOFT=1 | metric only (no force-fail) |
// | Production | default (sandbox not off)                    | success_flag=false + counter      |
// | Disabled   | AURA_MUTATION_HOLD_SLO_US=0                  | no check (AC4)                    |
// Happy path (hold ≤ SLO or disabled): one compare / getenv parse, zero force
// work beyond existing long-hold metrics (AC3).
//
// Default SLO 100_000 µs (100 ms). Live getenv (not process-static) so tests
// can set AURA_MUTATION_HOLD_SLO_US without process restart (same pattern as
// #2346 Soft/Hard).
[[nodiscard]] inline std::uint64_t mutation_hold_slo_us() noexcept {
    const char* e = std::getenv("AURA_MUTATION_HOLD_SLO_US");
    if (e == nullptr || e[0] == '\0')
        return 100000ULL; // production default 100ms
    // Explicit 0 disables the circuit (AC4).
    if (e[0] == '0' && e[1] == '\0')
        return 0;
    std::uint64_t v = 0;
    for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
        v = v * 10 + static_cast<std::uint64_t>(*p - '0');
    return v; // 0 if non-numeric → disable
}

// Soft / sandbox: metric-only SLO violation (AC2). Production (default): force.
[[nodiscard]] inline bool mutation_hold_slo_soft_mode() noexcept {
    const char* soft = std::getenv("AURA_MUTATION_HOLD_SLO_SOFT");
    if (soft && soft[0] == '1')
        return true;
    const char* sandbox = std::getenv("AURA_SANDBOX");
    return sandbox && sandbox[0] != '\0' && std::string_view(sandbox) == "off";
}

// ── Issue #2517: process-wide live longest outermost hold probe ──
// Best-effort CAS (AC5): under contention may lag one sample; Agents treat
// as soft real-time signal (not a hard mutex owner lock).
//
// Enter: claim empty slot, or replace if our start_ns is earlier (longer hold).
// Exit: if this fiber is the recorded max holder → clear (simplified; next
// enter rebuilds). Nested guards never touch the probe.

inline std::atomic<std::uint64_t> g_mutation_hold_live_fiber_id{0};
inline std::atomic<std::uint64_t> g_mutation_hold_live_start_ns{0};
inline std::atomic<std::uint32_t> g_mutation_hold_live_depth{0};
inline std::atomic<std::uint64_t> g_mutation_hold_live_update_total{0};
inline std::atomic<std::uint64_t> g_mutation_hold_live_clear_total{0};
inline std::atomic<std::uint64_t> g_mutation_hold_live_over_budget_observe_total{0};
inline std::atomic<std::uint32_t> g_mutation_hold_live_wired{1};

[[nodiscard]] inline std::uint64_t mutation_hold_steady_ns_now() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

[[nodiscard]] inline std::uint64_t
mutation_hold_steady_ns_of(std::chrono::steady_clock::time_point tp) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count());
}

// Outermost Guard enter: install or upgrade process-wide max-hold probe.
inline void mutation_hold_live_note_enter(std::uint64_t fiber_id, std::uint64_t start_ns,
                                          std::uint32_t depth) noexcept {
    if (fiber_id == 0)
        fiber_id = 1; // never store 0 as live id (0 = no holder)
    // Claim empty slot (fiber_id == 0).
    std::uint64_t expected_fid = 0;
    if (g_mutation_hold_live_fiber_id.compare_exchange_strong(
            expected_fid, fiber_id, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        g_mutation_hold_live_start_ns.store(start_ns, std::memory_order_release);
        g_mutation_hold_live_depth.store(depth, std::memory_order_relaxed);
        g_mutation_hold_live_update_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Occupied: replace only if our hold is longer (earlier start_ns).
    auto cur_start = g_mutation_hold_live_start_ns.load(std::memory_order_acquire);
    if (cur_start != 0 && start_ns < cur_start) {
        if (g_mutation_hold_live_start_ns.compare_exchange_strong(
                cur_start, start_ns, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            g_mutation_hold_live_fiber_id.store(fiber_id, std::memory_order_release);
            g_mutation_hold_live_depth.store(depth, std::memory_order_relaxed);
            g_mutation_hold_live_update_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Outermost Guard exit: clear if we are the recorded max holder.
inline void mutation_hold_live_note_exit(std::uint64_t fiber_id) noexcept {
    if (fiber_id == 0)
        fiber_id = 1;
    std::uint64_t expected = fiber_id;
    if (g_mutation_hold_live_fiber_id.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        g_mutation_hold_live_start_ns.store(0, std::memory_order_release);
        g_mutation_hold_live_depth.store(0, std::memory_order_relaxed);
        g_mutation_hold_live_clear_total.fetch_add(1, std::memory_order_relaxed);
    }
}

struct MutationHoldLiveSnapshot {
    std::uint64_t fiber_id = 0;
    std::uint64_t start_ns = 0;
    std::uint64_t duration_us = 0;
    std::uint32_t depth = 0;
    bool held = false;
};

// Pure read (+ optional over-budget observe counter). No holder → zeros (AC3).
[[nodiscard]] inline MutationHoldLiveSnapshot mutation_hold_live_snapshot() noexcept {
    MutationHoldLiveSnapshot s;
    s.fiber_id = g_mutation_hold_live_fiber_id.load(std::memory_order_acquire);
    s.start_ns = g_mutation_hold_live_start_ns.load(std::memory_order_acquire);
    s.depth = g_mutation_hold_live_depth.load(std::memory_order_relaxed);
    if (s.fiber_id != 0 && s.start_ns != 0) {
        s.held = true;
        const auto now = mutation_hold_steady_ns_now();
        if (now > s.start_ns)
            s.duration_us = (now - s.start_ns) / 1000ULL;
        // Optional agent-visible budget observe (does not force-fail).
        if (s.duration_us > mutation_hold_budget_us())
            g_mutation_hold_live_over_budget_observe_total.fetch_add(1, std::memory_order_relaxed);
    }
    return s;
}

// Test / process-reset seam (does not touch hold-estimate ring).
inline void mutation_hold_live_reset_for_test() noexcept {
    g_mutation_hold_live_fiber_id.store(0, std::memory_order_relaxed);
    g_mutation_hold_live_start_ns.store(0, std::memory_order_relaxed);
    g_mutation_hold_live_depth.store(0, std::memory_order_relaxed);
}

} // namespace aura::compiler

#endif // AURA_COMPILER_MUTATION_HOLD_BUDGET_H
