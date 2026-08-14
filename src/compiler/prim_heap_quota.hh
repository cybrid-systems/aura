// prim_heap_quota.hh — Issue #2916: soft heap quotas for core hot-path
// constructors under multi-fiber Agent self-evolution loops.
// Issue #2997: shorter lock holds + unlimited/small fast-path + Agent SLO.
//
// Limits are per-Evaluator absolute sizes of pairs_ / string_heap_ /
// vector_heap_ (0 = unlimited). Default unlimited preserves single-fiber
// hot-path latency for list-ref / member / math (those never call the
// allow helper).
//
// On breach, constructors return make_primitive_error (queryable) instead
// of unbounded growth / silent corruption under concurrent fibers.
//
// #2997 hot-path notes (no docs/design/ — #1655):
//   - list / append / reverse / map / json-array: reserve + one allow,
//     timed alloc_storage_lock_ (list_constructor_lock_hold_ns).
//   - unlimited + n <= kPrimHeapUnlimitedSmall: skip allow() (bypass).
//   - limit set: allow() still fail-closed; soft-hit when hw >= 70% limit.
//   - 4–8 fiber concurrent (list …) is measured in test_pmr_alloc_fiber_safe.
//
// Agent surfaces:
//   (resource:quota-set "pairs"|"strings"|"vectors" N)
//   (resource:quota-get "pairs"|"strings"|"vectors")
//   (engine:metrics "query:prim-heap-quota-stats")  schema-2916 + schema-2997
//
// See docs/stdlib/prim-heap-quota.md

#ifndef AURA_COMPILER_PRIM_HEAP_QUOTA_HH
#define AURA_COMPILER_PRIM_HEAP_QUOTA_HH

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aura::compiler {

inline constexpr int kPrimHeapQuotaIssue = 2916;
inline constexpr int kPrimHeapQuotaSchema = 2916;
// Issue #2997: lock-hold SLO + unlimited constructor fast-path.
inline constexpr int kPrimHeapQuotaHotPathIssue = 2997;
inline constexpr std::size_t kPrimHeapUnlimitedSmall = 8;
inline constexpr std::uint64_t kPrimHeapQuotaSoftHitBp = 7000; // 70% of limit
// Agent recommend codes on query:prim-heap-quota-stats.
inline constexpr std::int64_t kPrimHeapRecommendOk = 0;
inline constexpr std::int64_t kPrimHeapRecommendRaiseQuota = 1;
inline constexpr std::int64_t kPrimHeapRecommendShrinkFanout = 2;
// Avg lock-hold above this (ns) → recommend shrink-fanout.
inline constexpr std::uint64_t kPrimHeapLockHoldWarnNs = 50000;

// Absolute size soft limits for Evaluator heaps used by list/json/string/vector.
enum class PrimHeapDim : std::uint8_t {
    Pairs = 0,
    Strings = 1,
    Vectors = 2,
};

[[nodiscard]] inline std::string_view prim_heap_dim_name(PrimHeapDim d) noexcept {
    switch (d) {
        case PrimHeapDim::Pairs:
            return "pairs";
        case PrimHeapDim::Strings:
            return "strings";
        case PrimHeapDim::Vectors:
            return "vectors";
        default:
            return "unknown";
    }
}

// Error message fragment for make_primitive_error (stable Agent parse).
[[nodiscard]] inline std::string_view prim_heap_quota_exceeded_msg(PrimHeapDim d) noexcept {
    switch (d) {
        case PrimHeapDim::Pairs:
            return "prim-heap-quota: pairs soft limit exceeded";
        case PrimHeapDim::Strings:
            return "prim-heap-quota: strings soft limit exceeded";
        case PrimHeapDim::Vectors:
            return "prim-heap-quota: vectors soft limit exceeded";
        default:
            return "prim-heap-quota: soft limit exceeded";
    }
}

} // namespace aura::compiler

#endif // AURA_COMPILER_PRIM_HEAP_QUOTA_HH
