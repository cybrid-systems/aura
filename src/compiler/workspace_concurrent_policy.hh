// workspace_concurrent_policy.hh — Issue #2990: EDSL Workspace
// ConcurrentMutationPolicy (SingleWriter vs ScopedParallel).
//
// Complements #2976 AgentScope concurrency modes and #2985
// mutation-concurrency-health admit. Does NOT reimplement health
// throttle — ScopedParallel still calls maybe_reject_mutation_concurrency_health.
//
// Production default: SingleWriter (workspace_mtx_ unique, current
// try_acquire GlobalExclusive). Explicit opt-in ScopedParallel allows
// disjoint-subtree RegionExclusive (try_acquire_for_region + ImpactScope
// / dirty-cone proof). Overlap fail-closed falls back to SingleWriter.
//
// Env opt-in: AURA_WORKSPACE_CONCURRENT_MUTATION_POLICY=scoped-parallel|1

#ifndef AURA_COMPILER_WORKSPACE_CONCURRENT_POLICY_HH
#define AURA_COMPILER_WORKSPACE_CONCURRENT_POLICY_HH

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace aura::compiler {

enum class ConcurrentMutationPolicy : std::uint8_t {
    SingleWriter = 0,
    ScopedParallel = 1,
};

inline constexpr int kWorkspaceConcurrentPolicyIssue = 2990;

// Production default SingleWriter. Soft/dev may still opt in via env.
[[nodiscard]] inline ConcurrentMutationPolicy
resolve_concurrent_mutation_policy_default() noexcept {
    const char* e = std::getenv("AURA_WORKSPACE_CONCURRENT_MUTATION_POLICY");
    if (e == nullptr || e[0] == '\0')
        return ConcurrentMutationPolicy::SingleWriter;
    const std::string_view v{e};
    if (v == "scoped-parallel" || v == "ScopedParallel" || v == "1")
        return ConcurrentMutationPolicy::ScopedParallel;
    return ConcurrentMutationPolicy::SingleWriter;
}

[[nodiscard]] inline const char*
concurrent_mutation_policy_name(ConcurrentMutationPolicy p) noexcept {
    return p == ConcurrentMutationPolicy::ScopedParallel ? "scoped-parallel" : "single-writer";
}

} // namespace aura::compiler

#endif
