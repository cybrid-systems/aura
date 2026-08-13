// runtime_production_abi.cpp — Issue #2955 production ABI self-check.

#include "serve/runtime_production_abi.h"

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

    if (fail_bits != 0) {
        g_production_abi_selfcheck_last_fail_bits.store(fail_bits, std::memory_order_relaxed);
        g_production_abi_selfcheck_fail_total.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "FATAL: production ABI self-check failed (#2955) fail_bits=0x%llx — "
                     "multi-worker must link strong steal-complete / fiber evaluator_id / "
                     "mutation boundary held / depth-from-ptr (not fiber_bridge weak no-ops)\n",
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
