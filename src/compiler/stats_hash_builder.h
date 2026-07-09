// stats_hash_builder.h — Issue #877: shared FNV-1a + linear-probe hash insert
// for query:* stats primitives (dedup of copy-pasted insert_kv lambdas).
//
// Phase 1: header-only helpers. Call sites migrate incrementally.
#ifndef AURA_COMPILER_STATS_HASH_BUILDER_H
#define AURA_COMPILER_STATS_HASH_BUILDER_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aura::compiler::stats {

// FNV-1a 64-bit hash of a C string key (same constants as observability).
[[nodiscard]] inline std::uint64_t fnv1a_key(const char* k_str) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const char* p = k_str; *p; ++p)
        h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
    return h;
}

[[nodiscard]] inline std::uint8_t fingerprint(std::uint64_t h) noexcept {
    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
    if (fp == 0xFF)
        fp = 0xFE;
    return fp;
}

// Issue #878: load atomic or 0 when metrics pointer is null.
template <typename AtomicT> [[nodiscard]] inline std::uint64_t load_or_zero(AtomicT* p) noexcept {
    return p ? p->load(std::memory_order_relaxed) : 0;
}

template <typename AtomicT> inline void bump_or_skip(AtomicT* p, std::uint64_t n = 1) noexcept {
    if (p)
        p->fetch_add(n, std::memory_order_relaxed);
}

} // namespace aura::compiler::stats

#endif
