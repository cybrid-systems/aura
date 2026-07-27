// coercion_provenance_policy.hh — Issue #2102 / #2185
//
// Process-wide provenance-miss policy for CoercionMap apply.
// Header form so security_defaults.hh (and main) can flip reject-on-miss
// under production defaults without importing the coercion_map module.
//
// Defaults (process start / reset_for_test):
//   force_audit_on_provenance_miss = true
//   reject_apply_on_provenance_miss = false  (#2102 ergonomic soft apply)
//
// Issue #2185 production path (apply_production_security_defaults):
//   reject_apply_on_provenance_miss = true under Restricted/Strict
//   reject_apply_on_provenance_miss = false when AURA_SANDBOX=off (dev)
//   AURA_COERCION_PROVENANCE_REJECT=reject|soft|1|0 overrides when set

#ifndef AURA_COMPILER_COERCION_PROVENANCE_POLICY_HH
#define AURA_COMPILER_COERCION_PROVENANCE_POLICY_HH

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace aura::compiler {

// force_audit: note miss for MutationBoundary Full-path audit (default on).
// reject_apply: skip CoercionNode insert on incomplete chain (production on).
inline std::atomic<std::uint32_t> g_force_audit_on_provenance_miss{1};
inline std::atomic<std::uint32_t> g_reject_apply_on_provenance_miss{0};
inline std::atomic<std::uint64_t> g_coercion_provenance_miss_force_audit_total{0};
inline std::atomic<std::uint64_t> g_coercion_provenance_miss_reject_total{0};

// Issue #2185 stamp for query surfaces / Agent discovery.
inline constexpr int kCoercionProvenanceRejectProductionIssue = 2185;

inline void set_force_audit_on_provenance_miss(bool on) noexcept {
    g_force_audit_on_provenance_miss.store(on ? 1u : 0u, std::memory_order_relaxed);
}
inline void set_reject_apply_on_provenance_miss(bool on) noexcept {
    g_reject_apply_on_provenance_miss.store(on ? 1u : 0u, std::memory_order_relaxed);
}
[[nodiscard]] inline bool force_audit_on_provenance_miss() noexcept {
    return g_force_audit_on_provenance_miss.load(std::memory_order_relaxed) != 0;
}
[[nodiscard]] inline bool reject_apply_on_provenance_miss() noexcept {
    return g_reject_apply_on_provenance_miss.load(std::memory_order_relaxed) != 0;
}

// Thread-local: miss recorded during this fiber's apply → consumed on
// MutationBoundary exit (process-wide counters stay global).
inline thread_local bool s_provenance_miss_this_boundary = false;

inline void note_provenance_miss_for_boundary() noexcept {
    s_provenance_miss_this_boundary = true;
}
[[nodiscard]] inline bool provenance_miss_pending_for_boundary() noexcept {
    return s_provenance_miss_this_boundary;
}
// Returns true if a miss was noted since last consume; clears the flag.
[[nodiscard]] inline bool consume_provenance_miss_for_boundary() noexcept {
    const bool v = s_provenance_miss_this_boundary;
    s_provenance_miss_this_boundary = false;
    return v;
}

// Soft defaults (#2102 / #2185 AC3): force-audit on, reject off.
inline void reset_coercion_provenance_miss_policy_for_test() noexcept {
    g_force_audit_on_provenance_miss.store(1, std::memory_order_relaxed);
    g_reject_apply_on_provenance_miss.store(0, std::memory_order_relaxed);
    s_provenance_miss_this_boundary = false;
}

// Issue #2185: production defaults force reject-on-miss (forensic refuse).
// Dev path (sandbox off) keeps soft apply for iterative typecheck.
inline void apply_production_coercion_provenance_defaults(bool dev_sandbox_off) noexcept {
    set_force_audit_on_provenance_miss(true); // defense in depth (#2102)
    if (dev_sandbox_off)
        set_reject_apply_on_provenance_miss(false);
    else
        set_reject_apply_on_provenance_miss(true);
}

// Soft / reject canary override: returns true if env set a value.
// AURA_COERCION_PROVENANCE_REJECT=reject|1|true|on → reject
// AURA_COERCION_PROVENANCE_REJECT=soft|0|false|off → soft apply
inline bool apply_coercion_provenance_reject_env_override() noexcept {
    const char* e = std::getenv("AURA_COERCION_PROVENANCE_REJECT");
    if (!e || !*e)
        return false;
    // Minimal parse without string_view include for header lightness.
    if (e[0] == 'r' || e[0] == 'R' || e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' ||
        e[0] == 'Y' || (e[0] == 'o' && e[1] == 'n')) {
        set_reject_apply_on_provenance_miss(true);
        return true;
    }
    if (e[0] == 's' || e[0] == 'S' || e[0] == '0' || e[0] == 'f' || e[0] == 'F' || e[0] == 'n' ||
        e[0] == 'N' || (e[0] == 'o' && e[1] == 'f')) {
        set_reject_apply_on_provenance_miss(false);
        return true;
    }
    return false;
}

} // namespace aura::compiler

#endif // AURA_COMPILER_COERCION_PROVENANCE_POLICY_HH
