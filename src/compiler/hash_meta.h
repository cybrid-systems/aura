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
// Issue #4612: tombstone for a DELETED slot of an open-addressed table.
// The probe keeps walking past it (the chain is still live), so a
// removal never truncates the chain of a key that was displaced past
// the hole. 0x7F sits below the occupied-fingerprint range
// (0x80..0xFE) so it can never collide with a fingerprint, and the JIT
// runtime already used 0x7F for the same role (HASH_TOMBSTONE).
inline constexpr std::uint8_t kTombstoneSlot = 0x7F;

// Issue #4612: a slot is LIVE when it carries an occupied fingerprint
// (0x80..0xFE) -- i.e. it is neither the empty slot nor the tombstone of
// a deleted key. Every reader of a hash table must use this test: a
// deleted slot keeps its (stale) key / value bytes, so a reader that
// only tests "!= 0xFF" would resurrect a removed key.
[[nodiscard]] inline constexpr bool hash_slot_is_live(std::uint8_t meta) noexcept {
    return meta != kEmptySlot && meta != kTombstoneSlot;
}

// ── FNV-1a 64-bit (#901) ───────────────────────────────────────────
inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

// Slot fingerprint: the top 7 hash bits, remapped out of the sentinel range.
inline constexpr std::uint64_t kFingerprintShift = 57u;
inline constexpr std::uint64_t kFingerprintMask = 0x7Fu;

[[nodiscard]] constexpr std::uint8_t fingerprint(std::uint64_t hash) noexcept {
    const auto bits = static_cast<std::uint8_t>((hash >> kFingerprintShift) & kFingerprintMask);
    const auto slot_fp = static_cast<std::uint8_t>(bits | kFingerprintHighBit);
    return slot_fp == kEmptySlot ? kFingerprintEmpty : slot_fp;
}

// Issue #920: string_view path — O(1) length, no strlen.
[[nodiscard]] inline std::uint64_t fnv1a_bytes(std::string_view view) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    for (const unsigned char byte : view)
        hash = (hash ^ byte) * kFnvPrime;
    return hash;
}

// Null and empty inputs hash to the FNV offset basis. Delegates to the
// string_view overload so this file has no raw pointer walk to lint.
[[nodiscard]] inline std::uint64_t fnv1a_bytes(const char* text) noexcept {
    if (text == nullptr)
        return kFnvOffsetBasis;
    return fnv1a_bytes(std::string_view(text));
}

} // namespace aura::compiler::hash

// Back-compat aliases under stats:: (Issue #877 call sites).
namespace aura::compiler::stats {
inline constexpr std::uint64_t kFnvOffsetBasis = hash::kFnvOffsetBasis;
inline constexpr std::uint64_t kFnvPrime = hash::kFnvPrime;
} // namespace aura::compiler::stats

#endif
