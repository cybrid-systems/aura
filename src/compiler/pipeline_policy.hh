// pipeline_policy.hh — Issue #2213 production pipeline strictness.
//
// Controls silent tree-walker fallback that would abandon SoA + Impact +
// partial-relower under AI mutation load. Header form so main / tests /
// service share one definition without module-attachment linkage breaks.

#ifndef AURA_COMPILER_PIPELINE_POLICY_HH
#define AURA_COMPILER_PIPELINE_POLICY_HH

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace aura::compiler {

// Issue #2213: policy when needs_tree_walker_fallback would fire.
//   Allow     — legacy / unit-test default: take tree-walker silently
//   Forbidden — production hard-fail (preferred): never silent walker
//   ForceSoa  — continue IR/SoA path + metric (softer production option)
enum class TreeWalkerFallbackPolicy : std::uint8_t {
    Allow = 0,
    Forbidden = 1,
    ForceSoa = 2,
};

inline std::atomic<std::uint32_t>& g_tree_walker_fallback_policy_atomic() noexcept {
    static std::atomic<std::uint32_t> p{
        static_cast<std::uint32_t>(TreeWalkerFallbackPolicy::Allow)};
    return p;
}

[[nodiscard]] inline TreeWalkerFallbackPolicy tree_walker_fallback_policy() noexcept {
    return static_cast<TreeWalkerFallbackPolicy>(
        g_tree_walker_fallback_policy_atomic().load(std::memory_order_relaxed));
}

inline void set_tree_walker_fallback_policy(TreeWalkerFallbackPolicy p) noexcept {
    g_tree_walker_fallback_policy_atomic().store(static_cast<std::uint32_t>(p),
                                                 std::memory_order_relaxed);
}

// Alias: "production pipeline strict" ⇔ Forbidden (hard-fail).
[[nodiscard]] inline bool production_pipeline_strict() noexcept {
    return tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Forbidden;
}

inline void set_production_pipeline_strict(bool on) noexcept {
    set_tree_walker_fallback_policy(on ? TreeWalkerFallbackPolicy::Forbidden
                                       : TreeWalkerFallbackPolicy::Allow);
}

// Unit-test reset → permissive Allow (AC2).
inline void reset_tree_walker_fallback_policy_for_test() noexcept {
    set_tree_walker_fallback_policy(TreeWalkerFallbackPolicy::Allow);
}

// Apply policy from production security defaults + env.
//   AURA_SANDBOX=off → Allow (unit Soft ergonomics)
//   AURA_PIPELINE_STRICT=0|off|allow → Allow
//   AURA_PIPELINE_STRICT=force-soa|force_soa|soa → ForceSoa
//   AURA_PIPELINE_STRICT=1|true|strict|forbid|forbidden → Forbidden
//   no env: Forbidden when production (dev_off=false), else Allow
inline void apply_pipeline_strict_defaults(bool dev_sandbox_off) noexcept {
    const char* e = std::getenv("AURA_PIPELINE_STRICT");
    if (e && *e) {
        std::string_view v(e);
        if (v == "0" || v == "off" || v == "allow" || v == "false" || v == "no") {
            set_tree_walker_fallback_policy(TreeWalkerFallbackPolicy::Allow);
            return;
        }
        if (v == "force-soa" || v == "force_soa" || v == "soa" || v == "soft") {
            set_tree_walker_fallback_policy(TreeWalkerFallbackPolicy::ForceSoa);
            return;
        }
        // 1 / true / strict / forbid / forbidden / on / yes / unknown → Forbidden
        set_tree_walker_fallback_policy(TreeWalkerFallbackPolicy::Forbidden);
        return;
    }
    if (dev_sandbox_off)
        set_tree_walker_fallback_policy(TreeWalkerFallbackPolicy::Allow);
    else
        set_tree_walker_fallback_policy(TreeWalkerFallbackPolicy::Forbidden);
}

// Disposition when needs_tree_walker_fallback is true (caller already knows).
enum class TreeWalkerFallbackDisposition : std::uint8_t {
    TakeWalker = 0, // legacy: eval via tree-walker
    ContinueIr = 1, // ForceSoa: stay on IR/SoA path
    HardError = 2,  // Forbidden: return error to Agent
};

// Pure policy consult (no metrics). Callers bump metrics.
[[nodiscard]] inline TreeWalkerFallbackDisposition
tree_walker_fallback_disposition(bool needs_fallback) noexcept {
    if (!needs_fallback)
        return TreeWalkerFallbackDisposition::ContinueIr;
    switch (tree_walker_fallback_policy()) {
        case TreeWalkerFallbackPolicy::Allow:
            return TreeWalkerFallbackDisposition::TakeWalker;
        case TreeWalkerFallbackPolicy::ForceSoa:
            return TreeWalkerFallbackDisposition::ContinueIr;
        case TreeWalkerFallbackPolicy::Forbidden:
        default:
            return TreeWalkerFallbackDisposition::HardError;
    }
}

} // namespace aura::compiler

#endif // AURA_COMPILER_PIPELINE_POLICY_HH
