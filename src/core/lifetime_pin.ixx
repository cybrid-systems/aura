// lifetime_pin.ixx — Issue #1226 Phase 1: LifetimePin RAII + FFI ownership protocol scaffold.

module;

export module aura.core.lifetime_pin;

import std;

export namespace aura::core::lifetime {

inline constexpr int kLifetimePinPhase = 1;

struct LifetimePinStats {
    std::uint64_t pins = 0;
    std::uint64_t unpins = 0;
    std::uint64_t ffi_handoffs = 0;
};

inline LifetimePinStats g_lifetime_pin_stats{};

// RAII pin for external / FFI-owned buffers (Phase 1 counters only).
class LifetimePin {
public:
    LifetimePin() noexcept { ++g_lifetime_pin_stats.pins; }
    ~LifetimePin() noexcept { ++g_lifetime_pin_stats.unpins; }
    LifetimePin(const LifetimePin&) = delete;
    LifetimePin& operator=(const LifetimePin&) = delete;
    LifetimePin(LifetimePin&&) noexcept = default;
    LifetimePin& operator=(LifetimePin&&) noexcept = default;

    void mark_ffi_handoff() noexcept { ++g_lifetime_pin_stats.ffi_handoffs; }
};

} // namespace aura::core::lifetime
