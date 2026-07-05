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
#include <exception>
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

// Issue #498: use Evaluator::prim_registrar_with_meta() as register_with_spec.

namespace primitives_detail {

    // Issue #709: bump the aggregate fast-path counter only.
    // Retained for backward compatibility — callers that don't have
    // a slot in scope (rare; usually only in synthetic test
    // fixtures) keep working. New code should prefer
    // prim_record_fastpath_hit_for_slot which provides per-prim
    // breakdown.
    inline void prim_record_fastpath_hit(CompilerMetrics* m) noexcept {
        if (m)
            m->primitive_fastpath_hits_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #479: bump BOTH the aggregate counter and the
    // per-slot counter. The slow path (lazy-grow on first
    // hit for a new slot) may allocate and could throw
    // bad_alloc — we swallow it because observability is
    // advisory, never correctness-critical. The hot path
    // (slot < capacity) is lock-free and noexcept.
    //
    // Returns the post-increment per-slot count so callers
    // that want to log "first N hits" don't need a second
    // relaxed load. Not used by current callers but cheap
    // to expose.
    inline std::uint64_t prim_record_fastpath_hit_for_slot(CompilerMetrics* m,
                                                           std::size_t slot) noexcept {
        if (!m)
            return 0;
        m->primitive_fastpath_hits_total.fetch_add(1, std::memory_order_relaxed);
        try {
            return m->primitive_fastpath_hits_for_slot(slot).fetch_add(1,
                                                                       std::memory_order_relaxed) +
                   1;
        } catch (...) {
            // OOM in lazy-grow — swallow; aggregate counter is
            // already updated, per-slot breakdown is best-effort.
            return 0;
        }
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

    // Issue #615: PRIM_ERROR macro — wrap make_primitive_error with the
    // standard capture names so primitive error sites read as one-liners
    // instead of repeating the 4-arg call. Requires the enclosing lambda
    // (or function) to have in scope:
    //   - std::pmr::vector<std::string>&    string_heap
    //   - std::vector<EvalValue>&           error_values
    //   - std::atomic<std::uint64_t>*       primitive_error_counter
    // matching the register_*_primitives() signature in evaluator.ixx.
    //
    // Usage:
    //   add("regex-match?", [&string_heap, &error_values,
    //                        primitive_error_counter](std::span<const EvalValue> a) {
    //       ...
    //       } catch (...) {
    //           return PRIM_ERROR("regex-match?: invalid regular expression");
    //       }
    //   });
    //
    // Why a macro (not a free function): the 3 references above need to
    // come from the enclosing lambda capture list, which only the
    // preprocessor can substitute without dragging the call-site context
    // into a helper. A free helper would force every primitive to
    // forward `string_heap` / `error_values` / `primitive_error_counter`
    // through argument plumbing on every error site — a net regression
    // for readability. Macro keeps the call site `return PRIM_ERROR(msg);`
    // and the helper is the one place the 4-arg shape lives.
    //
    // Scope-limited note: this intentionally does NOT cover sites where
    // (a) the lambda doesn't already capture the 3 standard names (rare;
    // future follow-up may add typed overloads for those), or (b) the
    // site is intentionally silent (defensive best-effort fallback,
    // e.g. parsing-tolerant reflect helpers). Those carry a sibling
    // [SILENCE-PRIM-#615] comment near the catch instead.
#define PRIM_ERROR(MSG) \
    ::aura::compiler::primitives_detail::make_primitive_error( \
        string_heap, error_values, (MSG), primitive_error_counter)

} // namespace primitives_detail

} // namespace aura::compiler

#endif // AURA_COMPILER_PRIMITIVES_DETAIL_H