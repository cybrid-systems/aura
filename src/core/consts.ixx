// consts.ixx — numeric constants for module interface units.
//
// The plain headers (compiler/hash_meta.h, compiler/basis_points.h) are the
// source of truth for .cpp consumers. A module interface unit cannot include
// them in its global fragment when importers also include them textually
// (GCC reports a redefinition of the attached entity), so this module
// exposes the same values under its own namespace. Keep the numbers in sync:
// tests/python/test_magic_constants.py asserts equality with the headers.
module;

#include <cstdint>

export module aura.core.consts;

export namespace aura::core::consts {

// ── FNV-1a 64-bit (matches compiler/hash_meta.h) ───────────────────
inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

// ── Open-addressed table metadata (matches compiler/hash_meta.h) ───
inline constexpr std::uint8_t kEmptySlot = 0xFF;
inline constexpr std::uint8_t kFingerprintEmpty = 0xFE;
inline constexpr std::uint8_t kFingerprintHighBit = 0x80;
inline constexpr std::uint8_t kTombstoneSlot = 0x7F;

[[nodiscard]] constexpr std::uint8_t fingerprint(std::uint64_t hash) noexcept {
    const auto print = static_cast<std::uint8_t>(static_cast<std::uint8_t>((hash >> 57U) & 0x7FU) |
                                                 kFingerprintHighBit);
    return print == kEmptySlot ? kFingerprintEmpty : print;
}

// ── Basis points (matches compiler/basis_points.h) ─────────────────
inline constexpr std::int64_t kBasisPointScale = 10000;

[[nodiscard]] constexpr std::int64_t basis_points(std::uint64_t num, std::uint64_t den) noexcept {
    if (den == 0)
        return 0;
    return static_cast<std::int64_t>((num * static_cast<std::uint64_t>(kBasisPointScale)) / den);
}

[[nodiscard]] constexpr std::int64_t basis_points_vacuous_full(std::uint64_t num,
                                                               std::uint64_t den) noexcept {
    if (den == 0)
        return kBasisPointScale;
    return static_cast<std::int64_t>((num * static_cast<std::uint64_t>(kBasisPointScale)) / den);
}

} // namespace aura::core::consts
