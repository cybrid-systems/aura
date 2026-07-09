// stats_hash_builder.h — Issue #877: shared FNV-1a + linear-probe hash insert
// for query:* stats primitives (dedup of copy-pasted insert_kv lambdas).
//
// FNV / fingerprint constants: hash_meta.h (#901 / #908).
// Phase 1: header-only helpers. Call sites migrate incrementally.
#ifndef AURA_COMPILER_STATS_HASH_BUILDER_H
#define AURA_COMPILER_STATS_HASH_BUILDER_H

#include "hash_meta.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aura::compiler::stats {

// FNV-1a 64-bit hash of a C string key (canonical via hash_meta).
[[nodiscard]] inline std::uint64_t fnv1a_key(const char* k_str) noexcept {
    return hash::fnv1a_bytes(k_str);
}

[[nodiscard]] inline std::uint8_t fingerprint(std::uint64_t h) noexcept {
    return hash::fingerprint(h);
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
