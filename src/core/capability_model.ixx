// capability_model.ixx — Issues #1180/#1187/#1192 Phase 1: Capability + Effect scaffold.

module;

export module aura.core.capability_model;

import std;

export namespace aura::core::capability {

inline constexpr int kCapabilityModelPhase = 1;

// First-class effects bound to capabilities (Phase 1 enum; grant matrix follows).
enum class Effect : std::uint16_t {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    Exec = 1 << 2,
    Mutate = 1 << 3,
    Network = 1 << 4,
    Ffi = 1 << 5,
    Render = 1 << 6,
};

[[nodiscard]] constexpr Effect operator|(Effect a, Effect b) noexcept {
    return static_cast<Effect>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}

[[nodiscard]] constexpr bool has_effect(Effect set, Effect bit) noexcept {
    return (static_cast<std::uint16_t>(set) & static_cast<std::uint16_t>(bit)) != 0;
}

struct CapabilityGrant {
    std::string_view name;
    Effect effects = Effect::None;
    std::uint64_t tenant_id = 0;
};

} // namespace aura::core::capability
