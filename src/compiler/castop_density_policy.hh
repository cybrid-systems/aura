// castop_density_policy.hh — Issue #2287 / #2319 / #2358 / #2459 CastOp density.
//
// Soft path (default / AURA_SANDBOX=off): dens > budget → over_budget
// counter + annotation hint. No MutateTypeGate reject (AC1 of #2459).
//
// Hard path (AURA_CASTOP_DENSITY_HARD=1): dens > budget → force-JIT via
// HotUpdateRegistry (codegen degrade). Does NOT fail MutationBoundary /
// typed mutate on the first fire (AC3 of #2358 / AC4 of #2459).
//
// Production closed-loop (#2459): production_defaults_active() OR HARD
// env also take the force-JIT path. Consecutive over-budget dirty scopes
// with unannotated Dynamic residual accumulate a streak; at
// AURA_CASTOP_DENSITY_STREAK_GATE (default 2) Soft warns, Hard/production
// bumps castop_density_gate_reject_total (MutateTypeGate-aligned reject
// pressure). Under budget → streak resets (AC3).
//
// Under budget: single early return after enabled store (AC4 zero extra work).

#ifndef AURA_COMPILER_CASTOP_DENSITY_POLICY_HH
#define AURA_COMPILER_CASTOP_DENSITY_POLICY_HH

#include "aura_jit_bridge.h" // AotReloadFail
#include "hot_update_registry.hh"
#include "mutate_type_gate.hh"
#include "observability_metrics.h"
#include "typed_mutation_audit.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace aura::compiler::castop_density {

// Process-wide consecutive over-budget unannotated streak (#2459).
inline std::atomic<std::uint64_t>& g_density_streak() noexcept {
    static std::atomic<std::uint64_t> s{0};
    return s;
}

// Last gate-reject sticky signal for Agents / boundary consumers.
inline std::atomic<std::uint64_t>& g_gate_reject_total() noexcept {
    static std::atomic<std::uint64_t> s{0};
    return s;
}

// hard_override: -1 = read env, 0 = force soft, 1 = force hard (tests).
[[nodiscard]] inline bool hard_env_enabled(int hard_override = -1) noexcept {
    if (hard_override >= 0)
        return hard_override != 0;
    const char* e = std::getenv("AURA_CASTOP_DENSITY_HARD");
    return e != nullptr && (e[0] == '1' || e[0] == 't' || e[0] == 'T');
}

// production_override: -1 = read production_defaults_active(), 0/1 force.
[[nodiscard]] inline bool production_path_enabled(int production_override = -1) noexcept {
    if (production_override >= 0)
        return production_override != 0;
    return aura::compiler::typed_audit::production_defaults_active();
}

// Force-JIT path: HARD env OR production defaults (#2459 AC4 first response).
[[nodiscard]] inline bool force_jit_path_enabled(int hard_override = -1,
                                                 int production_override = -1) noexcept {
    return hard_env_enabled(hard_override) || production_path_enabled(production_override);
}

// Streak threshold for second-order MutateTypeGate pressure (default 2).
[[nodiscard]] inline std::uint64_t streak_gate_threshold() noexcept {
    const char* e = std::getenv("AURA_CASTOP_DENSITY_STREAK_GATE");
    if (!e || !*e)
        return 2;
    char* end = nullptr;
    const unsigned long v = std::strtoul(e, &end, 10);
    if (end == e || v == 0)
        return 2;
    return static_cast<std::uint64_t>(v);
}

// Unannotated Dynamic residual heuristic (#2319 lineage).
[[nodiscard]] inline bool unannotated_dynamic_residual(const CompilerMetrics& m) noexcept {
    const auto dead_elim = m.dead_coercion_elim_total.load(std::memory_order_relaxed);
    const auto castop_em = m.coercion_castop_emitted_total.load(std::memory_order_relaxed);
    return castop_em > dead_elim + 16;
}

struct ClosedLoopResult {
    bool force_jit = false;
    bool gate_reject = false;
    bool soft_warn = false;
    std::uint64_t streak = 0;
};

// Issue #2459: production closed-loop density policy.
// dens/budget already known; under budget resets streak and returns early.
inline ClosedLoopResult apply_density_closed_loop(CompilerMetrics& m, std::uint64_t dens,
                                                  std::uint64_t budget, bool unannotated_dynamic,
                                                  int hard_override = -1,
                                                  int production_override = -1) noexcept {
    ClosedLoopResult r;
    const bool hard_env = hard_env_enabled(hard_override);
    const bool prod = production_path_enabled(production_override);
    const bool force_path = hard_env || prod;
    m.castop_density_hard_enabled.store(force_path ? 1 : 0, std::memory_order_relaxed);
    m.castop_density_production_default_wired.store(1, std::memory_order_relaxed);

    // Under budget → reset streak (AC3); no force-JIT / gate.
    if (dens <= budget) {
        g_density_streak().store(0, std::memory_order_relaxed);
        m.castop_density_streak.store(0, std::memory_order_relaxed);
        r.streak = 0;
        return r;
    }

    // Soft-only path (#2287 / AC1): no force-JIT, no streak, no gate reject.
    if (!force_path) {
        r.streak = g_density_streak().load(std::memory_order_relaxed);
        m.castop_density_streak.store(r.streak, std::memory_order_relaxed);
        return r;
    }

    // Issue #2358 / #2459 step 1: force-JIT (codegen degrade; mutate may continue).
    m.castop_density_hard_action_total.fetch_add(1, std::memory_order_relaxed);
    m.castop_density_hard_wired.store(1, std::memory_order_relaxed);
    hot_update_registry().on_force_jit_for_reason(AotReloadFail::Other);
    r.force_jit = true;

    // #2319 lineage residual metric when unannotated Dynamic remains heavy.
    if (unannotated_dynamic) {
        m.castop_density_hard_reject_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #2459 step 2: streak only for unannotated over-budget dirty scopes.
    if (unannotated_dynamic) {
        const auto streak = g_density_streak().fetch_add(1, std::memory_order_relaxed) + 1;
        r.streak = streak;
        m.castop_density_streak.store(streak, std::memory_order_relaxed);
        const auto thr = streak_gate_threshold();
        if (streak >= thr) {
            // Soft MutateTypeGate mode → warning pressure (annotation-hint path).
            // Hard / production-locked → gate reject counters (closed-loop).
            const bool hard_gate = mutate_type_gate::is_hard() || prod || hard_env;
            if (hard_gate) {
                m.castop_density_gate_reject_total.fetch_add(1, std::memory_order_relaxed);
                g_gate_reject_total().fetch_add(1, std::memory_order_relaxed);
                mutate_type_gate::g_hard_type_error_reject_total.fetch_add(
                    1, std::memory_order_relaxed);
                r.gate_reject = true;
            } else {
                m.castop_density_soft_warn_total.fetch_add(1, std::memory_order_relaxed);
                r.soft_warn = true;
            }
        }
    } else {
        // Over budget but annotated/narrowed residual: keep force-JIT, no streak.
        r.streak = g_density_streak().load(std::memory_order_relaxed);
        m.castop_density_streak.store(r.streak, std::memory_order_relaxed);
    }
    return r;
}

// Apply HARD / production force-JIT policy after dens/budget are known.
// Returns true if a force-JIT action fired. First fire does not reject mutate
// (AC3 of #2358). Streak gate may set gate_reject on subsequent fires (#2459).
inline bool apply_hard_policy(CompilerMetrics& m, std::uint64_t dens, std::uint64_t budget,
                              int hard_override = -1) noexcept {
    const bool unann = unannotated_dynamic_residual(m);
    auto r = apply_density_closed_loop(m, dens, budget, unann, hard_override, /*production=*/-1);
    return r.force_jit;
}

// Test helper: reset process-wide streak between cases.
inline void reset_streak_for_test() noexcept {
    g_density_streak().store(0, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t density_streak() noexcept {
    return g_density_streak().load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t gate_reject_total() noexcept {
    return g_gate_reject_total().load(std::memory_order_relaxed);
}

} // namespace aura::compiler::castop_density

#endif // AURA_COMPILER_CASTOP_DENSITY_POLICY_HH
