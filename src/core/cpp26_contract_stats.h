// cpp26_contract_stats.h — Issue #742 / #2142: runtime observability for
// C++26 Contracts + consteval hot-path invariants (zero release cost).
//
// Plain header (not a module) so contract_handler.cpp, value_tags.h,
// arena.ixx, and pass_manager can all bump counters without crossing
// module boundaries.
//
// ── Issue #2142: unified hot-path contract policy ───────────────────────
// Single observe-first API for value/arena/ir_soa (and related) tight loops:
//
//   AURA_HOT_RECORD()           — always bump hotpath_invariant_hits_total
//   AURA_HOT_CHECK(expr)        — enforce or no-op per build policy
//   AURA_HOT_CONTRACT(expr)     — RECORD + CHECK (preferred one-liner)
//
// Build policy:
//   * Debug / non-NDEBUG (default) OR -DAURA_CONTRACTS_ENFORCE:
//       CHECK → contract_assert(expr)  (fail-closed on violation)
//   * Release (NDEBUG) without AURA_CONTRACTS_ENFORCE:
//       CHECK → no-op  (happy path pays only atomic record; no assert)
//   * Optional -DAURA_CONTRACTS_OBSERVE (with NDEBUG):
//       CHECK → if (!(expr)) record_contract_violation_hotpath() (no abort)
//
// Do NOT scatter bare contract_assert + record_hotpath_invariant_hit pairs
// on new hot paths — use AURA_HOT_CONTRACT / AURA_HOT_RECORD + AURA_HOT_CHECK.
//
#ifndef AURA_CORE_CPP26_CONTRACT_STATS_H
#define AURA_CORE_CPP26_CONTRACT_STATS_H

#include <atomic>
#include <cstdint>
// contract_assert for enforce builds (no-op path never needs it at runtime).
#if !defined(NDEBUG) || defined(AURA_CONTRACTS_ENFORCE) || defined(AURA_CONTRACTS_OBSERVE)
#include <contracts>
#endif

namespace aura::core::cpp26 {

inline constexpr int kHotContractUnifyIssue = 2142;

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
// Issue #1620: bumped to 77 (+12 Arena max/FlatAST dirty/NodeTag/Value
// Special encodings/SoAView phase consteval invariants).
inline constexpr std::int64_t kConstevalChecksTotal = 77;
// Approximate Contract pre/post/assert density across Arena + Value +
// Shape + dirty hot paths (manual inventory; Agents detect drift).
// Issue #1519: raised from 26 → 48 after hot-path Contract deepening.
// Issue #1620: raised 48 → 56 (FlatAST get/type_id + mark_dirty +
// shape bit-test + arena tier overflow path).
// Issue #2142: unified AURA_HOT_CONTRACT surface (value/arena/ir_soa).
inline constexpr std::int64_t kContractHotPathsShipped = 62;

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
// Issue #1620: Arena/Value/Shape/FlatAST hot-path Contracts expand flag.
inline std::atomic<std::uint64_t> hotpath_contracts_1620_active{1};
// Issue #2142: unified AURA_HOT_CONTRACT helper wired on primary hot paths.
inline std::atomic<std::uint64_t> hotpath_contracts_2142_active{1};
inline std::atomic<std::uint64_t> aura_hot_contract_wired{1};
inline std::atomic<std::uint64_t> arena_tier_contracts_active{1};
inline std::atomic<std::uint64_t> value_as_star_contracts_active{1};
inline std::atomic<std::uint64_t> shape_bit_test_contracts_active{1};
inline std::atomic<std::uint64_t> flatast_get_type_contracts_active{1};
inline std::atomic<std::uint64_t> contract_violation_hotpath_count{0};
// Issue #1466: hot-path consteval invariant hits — bumped each time a
// new consteval invariant is added. Mirrors kConstevalChecksTotal but
// observable at runtime via (query:cpp26-contracts-stats).
inline std::atomic<std::uint64_t> consteval_invariants_total{77};

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

// Issue #2142: release-observe sample (optional AURA_CONTRACTS_OBSERVE).
inline void observe_hot_contract_false() noexcept {
    record_contract_violation_hotpath();
}

} // namespace aura::core::cpp26

// ── Issue #2142: AURA_HOT_* macros (see file header policy) ───────────────
// Always record a hot-path invariant probe (relaxed atomic).
#define AURA_HOT_RECORD() ::aura::core::cpp26::record_hotpath_invariant_hit()

// Enforce / observe / ignore the predicate per build flags.
#if defined(AURA_CONTRACTS_ENFORCE) || (!defined(NDEBUG) && !defined(AURA_CONTRACTS_OBSERVE))
// Debug default + explicit enforce: fail-closed.
#define AURA_HOT_CHECK(expr) contract_assert(expr)
#elif defined(AURA_CONTRACTS_OBSERVE)
// Release observe: metrics only, no abort.
#define AURA_HOT_CHECK(expr)                                                                       \
    do {                                                                                           \
        if (!(expr))                                                                               \
            ::aura::core::cpp26::observe_hot_contract_false();                                     \
    } while (0)
#else
// Release (NDEBUG): zero assert cost on happy path.
#define AURA_HOT_CHECK(expr) ((void)0)
#endif

// Preferred one-liner: record + check.
#define AURA_HOT_CONTRACT(expr)                                                                    \
    do {                                                                                           \
        AURA_HOT_RECORD();                                                                         \
        AURA_HOT_CHECK(expr);                                                                      \
    } while (0)

#endif // AURA_CORE_CPP26_CONTRACT_STATS_H