// runtime_production_abi.h — Issue #2955 production startup strong-symbol ABI
// self-check for steal / mutation / GC residual hooks.
//
// Production multi-worker assumes strong C-linkage definitions for:
//   - aura_evaluator_on_steal_complete
//   - aura_fiber_evaluator_id_for_steal_safety
//   - aura_evaluator_mutation_boundary_held
//   - aura_evaluator_mutation_stack_depth_from_ptr (depth-from-ptr)
// Weak no-ops (fiber_bridge / light-link) re-open orphan GC defer and
// mid-mutation steal windows that #2721/#2901 hard-AND assume are wired.
//
// Soft / AURA_SANDBOX=off / unit light-link: no forced abort (weak ergonomics).
// Production defaults (sandbox ≠ off + production_defaults_active): refuse
// to enter multi-worker Ready if any strong identity marker is missing.
#pragma once

#include <atomic>
#include <cstdint>

namespace aura::serve {

inline constexpr int kProductionAbiSelfcheckIssue = 2955;

// Process-wide self-check results (additive observability).
inline std::atomic<std::uint64_t> g_production_abi_selfcheck_ok_total{0};
inline std::atomic<std::uint64_t> g_production_abi_selfcheck_fail_total{0};
inline std::atomic<std::uint32_t> g_production_abi_selfcheck_wired{1};
// Bit mask of last fail (bit0 steal-complete, bit1 eval-id, bit2 held, bit3 depth).
inline std::atomic<std::uint64_t> g_production_abi_selfcheck_last_fail_bits{0};

[[nodiscard]] inline std::uint64_t production_abi_selfcheck_ok_total_v_read() noexcept {
    return g_production_abi_selfcheck_ok_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t production_abi_selfcheck_fail_total_v_read() noexcept {
    return g_production_abi_selfcheck_fail_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t production_abi_selfcheck_wired_v_read() noexcept {
    return g_production_abi_selfcheck_wired.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t production_abi_selfcheck_last_fail_bits_v_read() noexcept {
    return g_production_abi_selfcheck_last_fail_bits.load(std::memory_order_relaxed);
}

// Pure gate: true when production self-check is required (not Soft / sandbox=off).
[[nodiscard]] bool production_abi_selfcheck_required() noexcept;

// Run self-check. When required and any strong marker missing:
//   - bumps fail_total + last_fail_bits
//   - prints FATAL to stderr
//   - aborts (never Soft-continue into multi-worker)
// When Soft / not required: returns true without abort (ok_total not forced).
// When required and all strong present: ok_total +1, returns true.
// Idempotent for ok path (may be called from main + scheduler init).
bool aura_runtime_require_production_abi() noexcept;

// Test reset (unit isolation).
inline void clear_production_abi_selfcheck_for_test() noexcept {
    g_production_abi_selfcheck_ok_total.store(0, std::memory_order_relaxed);
    g_production_abi_selfcheck_fail_total.store(0, std::memory_order_relaxed);
    g_production_abi_selfcheck_last_fail_bits.store(0, std::memory_order_relaxed);
}

} // namespace aura::serve

// Strong-identity markers (C ABI). Strong TUs return 1; weak stubs return 0.
// When both are linked, the strong definition wins (ELF weak resolution).
extern "C" int aura_abi_strong_steal_complete_v(void) noexcept;
extern "C" int aura_abi_strong_fiber_eval_id_v(void) noexcept;
extern "C" int aura_abi_strong_mutation_held_v(void) noexcept;
extern "C" int aura_abi_strong_mutation_depth_from_ptr_v(void) noexcept;

// C ABI entry for hosts that cannot attach aura::serve.
extern "C" int aura_runtime_require_production_abi_c(void) noexcept;
