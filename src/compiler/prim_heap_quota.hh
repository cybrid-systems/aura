// prim_heap_quota.hh — Issue #2916: soft heap quotas for core hot-path
// constructors under multi-fiber Agent self-evolution loops.
//
// Limits are per-Evaluator absolute sizes of pairs_ / string_heap_ /
// vector_heap_ (0 = unlimited). Default unlimited preserves single-fiber
// hot-path latency for list-ref / member / math (those never call the
// allow helper).
//
// On breach, constructors return make_primitive_error (queryable) instead
// of unbounded growth / silent corruption under concurrent fibers.
//
// Agent surfaces:
//   (resource:quota-set "pairs"|"strings"|"vectors" N)
//   (resource:quota-get "pairs"|"strings"|"vectors")
//   (engine:metrics "query:prim-heap-quota-stats")  schema-2916
//
// See docs/stdlib/prim-heap-quota.md

#ifndef AURA_COMPILER_PRIM_HEAP_QUOTA_HH
#define AURA_COMPILER_PRIM_HEAP_QUOTA_HH

#include <cstdint>
#include <string_view>

namespace aura::compiler {

inline constexpr int kPrimHeapQuotaIssue = 2916;
inline constexpr int kPrimHeapQuotaSchema = 2916;

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
