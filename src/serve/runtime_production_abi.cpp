// runtime_production_abi.cpp — Issue #2955 production ABI self-check.

#include "serve/runtime_production_abi.h"
#include "serve/steal_safety.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Strong in typed_mutation_audit hooks; weak no-op in fiber.cpp. Avoids
// pulling compiler module into light serve link units.
extern "C" int aura_production_defaults_active_probe() noexcept;

namespace aura::serve {
namespace {

    [[nodiscard]] bool sandbox_is_off() noexcept {
        const char* sb = std::getenv("AURA_SANDBOX");
        return sb != nullptr && sb[0] != '\0' && std::strcmp(sb, "off") == 0;
    }

} // namespace

bool production_abi_selfcheck_required() noexcept {
    // Soft / unit light-link ergonomics: AURA_SANDBOX=off opts out.
    if (sandbox_is_off())
        return false;
    // Production defaults active (apply_production_security_defaults) requires
    // strong steal/mutation/GC residual hooks before multi-worker Ready.
    if (aura_production_defaults_active_probe() == 0)
        return false;
    return true;
}

bool aura_runtime_require_production_abi() noexcept {
    if (!production_abi_selfcheck_required()) {
        // Soft path: zero forced work beyond the production_defaults load.
        return true;
    }

    std::uint64_t fail_bits = 0;
    if (aura_abi_strong_steal_complete_v() == 0)
        fail_bits |= 1ull << 0;
    if (aura_abi_strong_fiber_eval_id_v() == 0)
        fail_bits |= 1ull << 1;
    if (aura_abi_strong_mutation_held_v() == 0)
        fail_bits |= 1ull << 2;
    if (aura_abi_strong_mutation_depth_from_ptr_v() == 0)
        fail_bits |= 1ull << 3;
    // Issue #3275: tenant-scope resume ABI must be strong in production
    // (weak no-op would skip fiber-resume principal rebind silently).
    if (aura_abi_strong_tenant_scope_resume_v() == 0)
        fail_bits |= kProductionAbiSelfcheckFailBitTenantScope;
    // Issue #3343: steal linear-probe ABI must be strong in production
    // (weak empty body would skip ownership probe + escape clear).
    if (aura_abi_strong_probe_linear_on_steal_v() == 0)
        fail_bits |= kProductionAbiSelfcheckFailBitProbeLinear;
    // Issue #3419: JIT typed-entry must be the strong wrapper (weak stub
    // returning OK is not production).
    if (aura_abi_strong_ir_typed_entry_v() == 0)
        fail_bits |= kProductionAbiSelfcheckFailBitTypedEntry;

    if (fail_bits != 0) {
        g_production_abi_selfcheck_last_fail_bits.store(fail_bits, std::memory_order_relaxed);
        g_production_abi_selfcheck_fail_total.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(
            stderr,
            "FATAL: production ABI self-check failed (#2955/#3275/#3343/#3419) fail_bits=0x%llx "
            "— multi-worker must link strong steal-complete / fiber evaluator_id / "
            "mutation boundary held / depth-from-ptr / tenant-scope resume / "
            "probe-linear-on-steal / ir-typed-entry (not fiber_bridge / JIT stub weaks)\n",
            static_cast<unsigned long long>(fail_bits));
        std::fflush(stderr);
        std::abort();
    }

    g_production_abi_selfcheck_ok_total.fetch_add(1, std::memory_order_relaxed);
    g_production_abi_selfcheck_last_fail_bits.store(0, std::memory_order_relaxed);
    return true;
}

// Issue #3098: production multi-worker Ready self-check. AND-s strong
// ABI markers + production_defaults_active. Unlike the single-worker
// variant above, this function NEVER returns true without abort when
// Soft (sandbox=off) / !production_defaults_active / any strong marker
// missing. Closes the residual configuration hole where multi-worker
// processes were silently falling through to Soft residual arms
// (steal after densify miss, hold after cancel, query:*-stable pre-mutate
// gen) when sandbox=off or production_defaults_active was false.
//
// Single-worker / Soft unit / light-link callers should use the
// single-worker variant (aura_runtime_require_production_abi) which
// returns true under Soft without abort.
bool aura_runtime_require_production_multi_worker() noexcept {
    std::uint64_t fail_bits = 0;

    // Issue #3098 AC1: multi-worker refuses Soft fall-through.
    // sandbox=off OR !production_defaults_active → bit 4 + abort.
    if (sandbox_is_off()) {
        fail_bits |= kProductionAbiSelfcheckFailBitDefaults;
    }
    if (aura_production_defaults_active_probe() == 0) {
        fail_bits |= kProductionAbiSelfcheckFailBitDefaults;
    }

    // Issue #2955: existing ABI marker checks (bits 0-3).
    if (aura_abi_strong_steal_complete_v() == 0)
        fail_bits |= 1ull << 0;
    if (aura_abi_strong_fiber_eval_id_v() == 0)
        fail_bits |= 1ull << 1;
    if (aura_abi_strong_mutation_held_v() == 0)
        fail_bits |= 1ull << 2;
    if (aura_abi_strong_mutation_depth_from_ptr_v() == 0)
        fail_bits |= 1ull << 3;

    // Issue #3275: tenant-scope resume ABI must be strong under
    // multi-worker production (weak no-op would silently run fiber
    // resumes under the worker's ambient principal).
    if (aura_abi_strong_tenant_scope_resume_v() == 0)
        fail_bits |= kProductionAbiSelfcheckFailBitTenantScope;
    // Issue #3343: steal linear-probe ABI must be strong under
    // multi-worker production (weak empty body would skip ownership
    // probe + escape clear + invalidate_gen on steal).
    if (aura_abi_strong_probe_linear_on_steal_v() == 0)
        fail_bits |= kProductionAbiSelfcheckFailBitProbeLinear;
    // Issue #3419: JIT typed-entry strong wrapper required under
    // multi-worker production (weak stub returning OK is not production).
    if (aura_abi_strong_ir_typed_entry_v() == 0)
        fail_bits |= kProductionAbiSelfcheckFailBitTypedEntry;

    // Issue #3195: residual-zero sticky wiring must be present. Header
    // sentinels are 1 unless a mis-link / test store(0) wiped them.
    if (g_steal_safety_production_residual_sticky_fail_wired.load(std::memory_order_relaxed) == 0 ||
        g_steal_safety_production_residual_zero_wired.load(std::memory_order_relaxed) == 0)
        fail_bits |= kProductionAbiSelfcheckFailBitResidualSticky;

    if (fail_bits != 0) {
        g_production_abi_selfcheck_last_fail_bits.store(fail_bits, std::memory_order_relaxed);
        g_production_abi_selfcheck_fail_total.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(
            stderr,
            "FATAL: production multi-worker Ready self-check failed (#3098 + #2955 + #3195) "
            "fail_bits=0x%llx — multi-worker requires strong ABI + "
            "production_defaults_active + residual-zero sticky wiring. Soft "
            "(AURA_SANDBOX=off) / !production_defaults_active under multi-worker "
            "is refused.\n",
            static_cast<unsigned long long>(fail_bits));
        std::fflush(stderr);
        std::abort();
    }

    // Latch before residual_zero consult so a later Soft flip cannot
    // pass-through (I3/I6). steal_safety_production_residual_zero_v_read
    // remains SSOT for readiness.
    g_production_multi_worker_latched.store(1, std::memory_order_relaxed);

    if (steal_safety_production_residual_zero_v_read() == 0) {
        fail_bits |= kProductionAbiSelfcheckFailBitResidualSticky;
        g_production_abi_selfcheck_last_fail_bits.store(fail_bits, std::memory_order_relaxed);
        g_production_abi_selfcheck_fail_total.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "FATAL: production multi-worker Ready residual-zero sticky failed (#3195) "
                     "fail_bits=0x%llx — named residuals must be 0 at multi-worker Ready.\n",
                     static_cast<unsigned long long>(fail_bits));
        std::fflush(stderr);
        std::abort();
    }

    g_production_abi_selfcheck_ok_total.fetch_add(1, std::memory_order_relaxed);
    g_production_abi_selfcheck_last_fail_bits.store(0, std::memory_order_relaxed);
    return true;
}

} // namespace aura::serve

extern "C" int aura_runtime_require_production_abi_c(void) noexcept {
    return aura::serve::aura_runtime_require_production_abi() ? 1 : 0;
}
// Issue #3098: C-linkage accessor for multi-worker Ready self-check.
extern "C" int aura_runtime_require_production_multi_worker_c(void) noexcept {
    return aura::serve::aura_runtime_require_production_multi_worker() ? 1 : 0;
}

extern "C" int aura_runtime_multi_worker_production_latched(void) noexcept {
    return aura::serve::g_production_multi_worker_latched.load(std::memory_order_relaxed) != 0 ? 1
                                                                                               : 0;
}
