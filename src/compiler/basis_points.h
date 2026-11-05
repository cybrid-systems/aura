// basis_points.h — Issue #905: percentage as basis points (0–10000).
#ifndef AURA_COMPILER_BASIS_POINTS_H
#define AURA_COMPILER_BASIS_POINTS_H

#include <cstdint>

namespace aura::compiler {

// 10000 = 100.00%. Use for all (num*scale)/den percentage fields.
inline constexpr std::int64_t kBasisPointScale = 10000;

// Basis-point rate for a measured population. An empty denominator means
// "no samples yet", so the result is 0 (no data), never a vacuous 100%.
[[nodiscard]] inline constexpr std::int64_t basis_points(std::uint64_t num,
                                                         std::uint64_t den) noexcept {
    if (den == 0)
        return 0;
    return static_cast<std::int64_t>((num * static_cast<std::uint64_t>(kBasisPointScale)) / den);
}

// Only for predicates that are vacuously true over an empty set (e.g.
// "every recorded sample was complete" with zero samples). Also use it for
// the N/A=perfect convention on health/adaptive inputs, where a fresh process
// must not look degraded before any sample exists. Informational rates use
// basis_points().
[[nodiscard]] inline constexpr std::int64_t basis_points_vacuous_full(std::uint64_t num,
                                                                      std::uint64_t den) noexcept {
    if (den == 0)
        return kBasisPointScale;
    return static_cast<std::int64_t>((num * static_cast<std::uint64_t>(kBasisPointScale)) / den);
}

} // namespace aura::compiler

#endif
