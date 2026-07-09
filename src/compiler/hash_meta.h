// hash_meta.h — Issue #908 / #901: open-addressed hash table sentinels + FNV-1a.
//
// Canonical home for linear-probe metadata markers and 64-bit FNV-1a
// constants shared by stats builders, JIT runtime, and observability.
#ifndef AURA_COMPILER_HASH_META_H
#define AURA_COMPILER_HASH_META_H

#include <cstdint>
#include <string_view>

namespace aura::compiler::hash {

// ── Linear-probe metadata sentinels (#908) ─────────────────────────
// 0xFF = empty slot (probe stops). Fingerprints never use 0xFF.
inline constexpr std::uint8_t kEmptySlot = 0xFF;
// Remap when (h>>57)&0x7F|0x80 would equal kEmptySlot.
inline constexpr std::uint8_t kFingerprintEmpty = 0xFE;
// Occupied fingerprints have the high bit set.
inline constexpr std::uint8_t kFingerprintHighBit = 0x80;

// ── FNV-1a 64-bit (#901) ───────────────────────────────────────────
inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

[[nodiscard]] constexpr std::uint8_t fingerprint(std::uint64_t h) noexcept {
    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | kFingerprintHighBit;
    return fp == kEmptySlot ? kFingerprintEmpty : fp;
}

[[nodiscard]] inline std::uint64_t fnv1a_bytes(const char* p) noexcept {
    std::uint64_t h = kFnvOffsetBasis;
    for (; p && *p; ++p)
        h = (h ^ static_cast<std::uint8_t>(*p)) * kFnvPrime;
    return h;
}

// Issue #920: string_view path — O(1) length, no strlen.
[[nodiscard]] inline std::uint64_t fnv1a_bytes(std::string_view sv) noexcept {
    std::uint64_t h = kFnvOffsetBasis;
    for (unsigned char c : sv)
        h = (h ^ c) * kFnvPrime;
    return h;
}

} // namespace aura::compiler::hash

// Back-compat aliases under stats:: (Issue #877 call sites).
namespace aura::compiler::stats {
inline constexpr std::uint64_t kFnvOffsetBasis = hash::kFnvOffsetBasis;
inline constexpr std::uint64_t kFnvPrime = hash::kFnvPrime;
} // namespace aura::compiler::stats

#endif
