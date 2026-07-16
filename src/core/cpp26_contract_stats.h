// cpp26_contract_stats.h — Issue #742: runtime observability for
// C++26 Contracts + consteval hot-path invariants (zero release cost).
//
// Plain header (not a module) so contract_handler.cpp, value_tags.h,
// arena.ixx, and pass_manager can all bump counters without crossing
// module boundaries.
#ifndef AURA_CORE_CPP26_CONTRACT_STATS_H
#define AURA_CORE_CPP26_CONTRACT_STATS_H

#include <atomic>
#include <cstdint>

namespace aura::core::cpp26 {

// Runtime contract violations caught by handle_contract_violation
// (enforce/observe semantic). Stats-only; relaxed ordering.
inline std::atomic<std::uint64_t> contract_violations_caught_total{0};

// Hot-path invariant probes (Arena alloc, Value classify, SoA view,
// Shape inline, Pass dirty-skip). Zero cost in release — advisory only.
inline std::atomic<std::uint64_t> hotpath_invariant_hits_total{0};

// Compile-time consteval/static_assert count baked into the binary.
// Bump when cxx26_invariants.ixx / value_tags.h / shape.h grow.
// Issue #1321: expanded to 36 (+4 dirty/tag/arena packing asserts).
// Issue #1466: bumped to 53 (+17 hot-path consteval invariants:
// EvalValueTag enum x9 + ShapeID boundary x4 + IR SoA breakdown x3 +
// tagged bit layout x1).
// Issue #1519: bumped to 65 (+12 SIMD/cache/dirty/shape/freelist asserts).
inline constexpr std::int64_t kConstevalChecksTotal = 65;
// Approximate Contract pre/post/assert density across Arena + Value +
// Shape + dirty hot paths (manual inventory; Agents detect drift).
// Issue #1519: raised from 26 → 48 after hot-path Contract deepening.
inline constexpr std::int64_t kContractHotPathsShipped = 48;

// Issue #1321 Phase 1: coverage flags — hot accessors that gained contracts.
inline std::atomic<std::uint64_t> hotpath_contracts_expanded_active{1};
inline std::atomic<std::uint64_t> soa_view_bounds_contracts_active{1};
inline std::atomic<std::uint64_t> flatast_column_contracts_active{1};

// Issue #1466 Phase 1: new coverage flags — hot-path contract placement.
inline std::atomic<std::uint64_t> shape_inline_post_contracts_active{1};
inline std::atomic<std::uint64_t> arena_compact_contracts_active{1};
inline std::atomic<std::uint64_t> dirty_cascade_contracts_active{1};
// Issue #1519: deeper hot-path Contracts coverage flag + violation surface.
inline std::atomic<std::uint64_t> hotpath_contracts_1519_active{1};
inline std::atomic<std::uint64_t> contract_violation_hotpath_count{0};
// Issue #1466: hot-path consteval invariant hits — bumped each time a
// new consteval invariant is added. Mirrors kConstevalChecksTotal but
// observable at runtime via (query:cpp26-contracts-stats).
inline std::atomic<std::uint64_t> consteval_invariants_total{65};

inline void record_contract_violation_caught() noexcept {
    contract_violations_caught_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_hotpath_invariant_hit() noexcept {
    hotpath_invariant_hits_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #1519: hot-path contract violation (debug observe path / Agent signal).
inline void record_contract_violation_hotpath() noexcept {
    contract_violation_hotpath_count.fetch_add(1, std::memory_order_relaxed);
    record_contract_violation_caught();
}

// Issue #1466: bump the consteval invariant count when new invariants
// are added in cxx26_invariants.ixx. Called from the consteval
// self-check initialization (not from hot path — one-shot at boot).
inline void record_consteval_invariant_added() noexcept {
    consteval_invariants_total.fetch_add(1, std::memory_order_relaxed);
}

} // namespace aura::core::cpp26

#endif // AURA_CORE_CPP26_CONTRACT_STATS_H