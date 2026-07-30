// castop_density_policy.hh — Issue #2287 / #2319 / #2358 CastOp density budget.
//
// Soft path (default): dens > budget → over_budget counter + annotation hint.
// Hard path (AURA_CASTOP_DENSITY_HARD=1): dens > budget → force-JIT via
// HotUpdateRegistry (codegen degrade). Does NOT fail MutationBoundary /
// typed mutate (AC3 of #2358).
//
// Under budget: single early return after enabled store (AC4 zero extra work).

#ifndef AURA_COMPILER_CASTOP_DENSITY_POLICY_HH
#define AURA_COMPILER_CASTOP_DENSITY_POLICY_HH

#include "aura_jit_bridge.h" // AotReloadFail
#include "hot_update_registry.hh"
#include "observability_metrics.h"

#include <cstdint>
#include <cstdlib>

namespace aura::compiler::castop_density {

// hard_override: -1 = read env, 0 = force soft, 1 = force hard (tests).
[[nodiscard]] inline bool hard_env_enabled(int hard_override = -1) noexcept {
    if (hard_override >= 0)
        return hard_override != 0;
    const char* e = std::getenv("AURA_CASTOP_DENSITY_HARD");
    return e != nullptr && (e[0] == '1' || e[0] == 't' || e[0] == 'T');
}

// Apply HARD policy after dens/budget are known. Returns true if a hard
// force-JIT action fired. Mutate always continues regardless (AC3).
inline bool apply_hard_policy(CompilerMetrics& m, std::uint64_t dens, std::uint64_t budget,
                              int hard_override = -1) noexcept {
    const bool hard = hard_env_enabled(hard_override);
    m.castop_density_hard_enabled.store(hard ? 1 : 0, std::memory_order_relaxed);
    // AC4: under budget or HARD off → no force-JIT / action bump.
    if (!hard || dens <= budget)
        return false;

    // Issue #2358: force-JIT (codegen degrade, not type gate).
    m.castop_density_hard_action_total.fetch_add(1, std::memory_order_relaxed);
    m.castop_density_hard_wired.store(1, std::memory_order_relaxed);
    // AotReloadFail::Other = density policy (no enum ABI bump).
    hot_update_registry().on_force_jit_for_reason(AotReloadFail::Other);

    // Issue #2319 lineage: unannotated Dynamic residual metric.
    const auto dead_elim = m.dead_coercion_elim_total.load(std::memory_order_relaxed);
    const auto castop_em = m.coercion_castop_emitted_total.load(std::memory_order_relaxed);
    if (castop_em > dead_elim + 16) {
        m.castop_density_hard_reject_total.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

} // namespace aura::compiler::castop_density

#endif // AURA_COMPILER_CASTOP_DENSITY_POLICY_HH
