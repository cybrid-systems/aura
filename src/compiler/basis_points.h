// basis_points.h — Issue #905: percentage as basis points (0–10000).
#ifndef AURA_COMPILER_BASIS_POINTS_H
#define AURA_COMPILER_BASIS_POINTS_H

#include <cstdint>

namespace aura::compiler {

// 10000 = 100.00%. Use for all (num*scale)/den percentage fields.
inline constexpr std::int64_t kBasisPointScale = 10000;

// Safe basis-point rate: 0 when den==0.
[[nodiscard]] inline constexpr std::int64_t basis_points(std::uint64_t num,
                                                         std::uint64_t den) noexcept {
    if (den == 0)
        return 0;
    return static_cast<std::int64_t>((num * static_cast<std::uint64_t>(kBasisPointScale)) / den);
}

} // namespace aura::compiler

#endif
