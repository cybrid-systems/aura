// primitives_detail.h — Issue #709: lambda capture discipline + fast dispatch helpers
//
// PRIM_CAPTURE_CONTRACT (enforced by convention + review, runtime telemetry):
//   - Mutate paths: capture [&ev], use MutationBoundaryGuard for provenance.
//   - Error paths: pass primitive_error_counter to make_primitive_error.
//   - Read-only hot paths: capture [&ev] or explicit heap refs; no silent catch.
//
// Agents extending stdlib should use DEFINE_PRIMITIVE_META for registration meta
// and slot_lookup_fast (via prim_record_fastpath_hit) in list/map/filter loops.

#ifndef AURA_COMPILER_PRIMITIVES_DETAIL_H
#define AURA_COMPILER_PRIMITIVES_DETAIL_H

#include "observability_metrics.h"
#include "primitives_meta.h"

#include <atomic>
#include <cstdint>
namespace aura::compiler {

// Bump when extension-kit capture contract version changes.
inline constexpr int kPrimCaptureContractVersion = 1;

// C++ registration macro — use inside aura.compiler.evaluator partitions where
// PrimMeta is in scope. Mirrors skeleton strings in primitives_meta.h (#697).
#define DEFINE_PRIMITIVE_META(ARITY, PURE, SAFETY, CATEGORY, DOC, SCHEMA)                          \
    ::aura::compiler::PrimMeta {                                                                   \
        .arity = static_cast<std::uint8_t>(ARITY), .pure = (PURE),                                 \
        .safety_flags = static_cast<std::uint8_t>(SAFETY), .doc = DOC, .category = CATEGORY,       \
        .schema = SCHEMA                                                                           \
    }

namespace primitives_detail {

    inline void prim_record_fastpath_hit(CompilerMetrics* m) noexcept {
        if (m)
            m->primitive_fastpath_hits_total.fetch_add(1, std::memory_order_relaxed);
    }

    inline void prim_record_capture_violation(CompilerMetrics* m) noexcept {
        if (m)
            m->primitive_capture_violations_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Runtime consistency probe: mutate primitives should wire error_counter;
    // callers pass has_error_counter=false when a primitive swallows errors.
    inline void prim_check_capture_contract(CompilerMetrics* m, bool has_error_counter,
                                            bool uses_guard_provenance) noexcept {
        (void)uses_guard_provenance;
        if (!has_error_counter)
            prim_record_capture_violation(m);
    }

} // namespace primitives_detail

} // namespace aura::compiler

#endif // AURA_COMPILER_PRIMITIVES_DETAIL_H