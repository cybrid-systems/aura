// ═══════════════════════════════════════════════════════════════════
// MODULE BOUNDARY (Issue #58)
// ═══════════════════════════════════════════════════════════════════
//
// value.ixx is a `module;` followed by `#include "value_tags.h"`.
// The header carries the tag/bias constants and the RefType ids that
// are shared with lib/runtime.c (a traditional .c file that has no
// knowledge of C++26 modules). Those constants are intentionally NOT
// `export`ed from this module — they are implementation details of
// the tagged-value encoding.

// Re-export is_truthy from aura::compiler::evaluator_pure for
// the legacy types::is_truthy callers in evaluator_impl.cpp.
// (Issue #146 Phase 3: is_truthy moved to the pure module.)
// export import aura.compiler.evaluator_pure;  // REMOVED: caused
// circular import (evaluator_pure → value → evaluator_pure).
//
// What's EXPORTED from this module:
//   * EvalValue struct and its operator== / operator<=>
//   * make_int / make_float / make_string / make_* helpers
//   * is_int / is_float / is_string / is_* inspectors
//   * as_int / as_string / as_* accessors
//
// What must be accessed via value_tags.h (NOT the module interface):
//   * FLOAT_BIAS_VAL, STRING_BIAS_VAL  (raw bias sentinels)
//   * make_string_raw, make_ref         (low-level encoders)
//   * is_fixnum, is_ref, is_special     (bit-pattern inspectors)
//   * RefPair / RefClosure / ...        (RefType ids)
//
// Why this split? If we `export` FLOAT_BIAS_VAL, every .cpp that
// imports this module gets a constexpr std::int64_t. The header
// path is cheaper for lib/runtime.c, JIT runtime C++, and any
// non-module consumer.
//
// Related: Issue #58 (archived: git tag docs-archive-pre-2026-06)
//
module;
#include "value_tags.h"
export module aura.compiler.value;
import std;

namespace aura::compiler::types {

// ═══════════════════════════════════════════════════════════
// Unified tagged value representation (matches AOT runtime tagging)
// ═══════════════════════════════════════════════════════════
//
// Bit encoding is defined in value_tags.h. See the header for the
// rationale on why these constants live in a traditional header
// instead of being `export`ed from this module (issue #58).
//
//   bits 0-1: TAG
//     00 = Fixnum (signed integer, value >> 1)
//     01 = Ref    (pool-indexed heap type)
//     11 = Special (#f=3, #t=7, void=11)
//
//   Ref sub-type (bits 2-5, 4 bits = 16 types): see value_tags.h
//   Ref bit layout: (pool_index << 6) | (type << 2) | 1
//
//   Float: stored in runtime pool with FLOAT_BIAS encoding
//   String: stored in runtime pool with STRING_BIAS encoding
//
// ═══════════════════════════════════════════════════════════

// The universal runtime value — a single int64_t with pointer tagging
export struct EvalValue {
    std::int64_t val = 0;

    constexpr EvalValue() noexcept
        : val(0) {}
    constexpr explicit EvalValue(std::int64_t v) noexcept
        : val(v) {}

    constexpr bool operator==(const EvalValue& o) const noexcept = default;
    constexpr auto operator<=>(const EvalValue& o) const noexcept = default;

    explicit operator bool() const = delete; // prevent implicit boolean conversion
    explicit operator std::int64_t() const noexcept { return val; }
};

// ── Float helpers (extern "C" runtime functions) ──────
// These are provided by lib/runtime.c
extern "C" {
std::int64_t aura_alloc_float(double d);
double aura_float_ref(std::int64_t val);
}

// ── make_* / is_* / as_* helpers ──────────────────────
// All keep the same signature as before — this is a transparent refactor.

export inline EvalValue make_int(std::int64_t v) noexcept {
    return EvalValue(v << 1); // fixnum encoding
}
export inline bool is_int(const EvalValue& v) noexcept {
    // Fixnum: bit0=0, but STRING_BIAS and FLOAT_BIAS values also have bit0=0.
    // Exclude both ranges to avoid type confusion.
    return is_fixnum(v.val) && v.val > FLOAT_BIAS_VAL;
}
export inline std::int64_t as_int(const EvalValue& v) noexcept {
    return v.val >> 1;
}

export inline EvalValue make_bool(bool v) noexcept {
    return EvalValue(v ? 7 : 3); // #t=7, #f=3
}
export inline bool is_bool(const EvalValue& v) noexcept {
    return v.val == 3 || v.val == 7;
}
export inline bool as_bool(const EvalValue& v) noexcept {
    return v.val == 7;
}

export inline EvalValue make_void() noexcept {
    return EvalValue(11); // void sentinel = 11
}
export inline bool is_void(const EvalValue& v) noexcept {
    return v.val == 11; // void sentinel = 11
}

export inline EvalValue make_float(double d) {
    return EvalValue(aura_alloc_float(d)); // FLOAT_BIAS encoding
}
export inline bool is_float(const EvalValue& v) noexcept {
    // Issue #181 Cycle 2: upper bound of float range is
    // STRING_BIAS_VAL_2 (the new string upper bound), not
    // STRING_BIAS_VAL. With the v2 string encoding,
    // string values can be at STRING_BIAS_VAL_2, so the
    // float range ends just below that.
    return v.val <= FLOAT_BIAS_VAL && v.val > STRING_BIAS_VAL_2;
}
export inline double as_float(const EvalValue& v) {
    return aura_float_ref(v.val);
}

// Issue #181 Cycle 2: string encoding migrated to Option A.
// make_string / is_string / as_string_idx now use the v2
// encoding (dedicated (v & 3) == 2 tag + range check against
// STRING_BIAS_VAL_2). The previous encoding was susceptible
// to collisions at idx ≡ 31 (mod 64) → RefError and
// idx ≡ 19 (mod 64) → RefKeyword. The new encoding is
// collision-free at the source.
export inline EvalValue make_string(std::uint64_t idx) noexcept {
    return EvalValue(make_string_raw_v2(idx));
}
export inline bool is_string(const EvalValue& v) noexcept {
    // Tag check is necessary (the bug fix) but not sufficient
    // (fixnums in the right range with the right bit pattern
    // would also pass the pure tag check). The range check
    // is the safety belt that prevents false positives.
    return is_string_raw_v2(v.val) && v.val <= STRING_BIAS_VAL_2;
}
export inline std::uint64_t as_string_idx(const EvalValue& v) noexcept {
    return string_idx_raw_v2(v.val);
}

// ── Issue #181 Cycle 1: v2 string encoding prototype ──────
//
// All string values have (v & 3) == 2 — disjoint from fixnum
// (0), ref (1), and special (3). Eliminates the
// idx ≡ 31 (mod 64) → RefError and
// idx ≡ 19 (mod 64) → RefKeyword collisions at the source.
//
// These prototypes are for testing/migration only. The full
// migration (replacing is_string, is_float, JIT emitter, etc.)
// is the work of Cycle 2 — see Issue #181 (archived: docs-archive-pre-2026-06).
export inline EvalValue make_string_v2(std::uint64_t idx) noexcept {
    return EvalValue(make_string_raw_v2(idx));
}
export inline bool is_string_v2(const EvalValue& v) noexcept {
    // Tag check is necessary (the bug fix) but not sufficient
    // (fixnums in the right range with the right bit pattern
    // would also pass the pure tag check). For the Cycle 1
    // prototype, keep the range check as a safety belt.
    // Cycle 2 may remove it once the encoding is fully
    // migrated and the bit allocation is documented as
    // reserved for strings.
    return is_string_raw_v2(v.val) && v.val <= STRING_BIAS_VAL_2;
}
export inline std::uint64_t as_string_idx_v2(const EvalValue& v) noexcept {
    return string_idx_raw_v2(v.val);
}

export inline EvalValue make_pair(std::uint64_t idx) noexcept {
    return EvalValue(make_ref(RefPair, idx));
}
export inline bool is_pair(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefPair;
}
export inline std::uint64_t as_pair_idx(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_closure(std::uint64_t id) noexcept {
    return EvalValue(make_ref(RefClosure, id));
}
export inline bool is_closure(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefClosure;
}
export inline std::uint64_t as_closure_id(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_cell(std::uint64_t id) noexcept {
    return EvalValue(make_ref(RefCell, id));
}
export inline bool is_cell(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefCell;
}
export inline std::uint64_t as_cell_id(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_vector(std::uint64_t idx) noexcept {
    return EvalValue(make_ref(RefVector, idx));
}
export inline bool is_vector(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefVector;
}
export inline std::uint64_t as_vector_idx(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_hash(std::uint64_t idx) noexcept {
    return EvalValue(make_ref(RefHash, idx));
}
export inline bool is_hash(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefHash;
}
export inline std::uint64_t as_hash_idx(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_primitive(std::size_t slot) noexcept {
    return EvalValue(make_ref(RefPrimitive, static_cast<std::uint64_t>(slot)));
}
export inline bool is_primitive(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefPrimitive;
}
export inline std::size_t as_primitive_slot(const EvalValue& v) noexcept {
    return static_cast<std::size_t>(ref_index(v.val));
}

export inline EvalValue make_module(std::uint64_t idx) noexcept {
    return EvalValue(make_ref(RefModule, idx));
}
export inline bool is_module(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefModule;
}
export inline std::uint64_t as_module_idx(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_error(std::uint64_t idx) noexcept {
    return EvalValue(make_ref(RefError, idx));
}
export inline bool is_error(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefError;
}
export inline std::uint64_t as_error_idx(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_opaque(std::uint64_t idx) noexcept {
    return EvalValue(make_ref(RefOpaque, idx));
}
export inline bool is_opaque(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefOpaque;
}
export inline std::uint64_t as_opaque_idx(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_linear(std::uint64_t id) noexcept {
    return EvalValue(make_ref(RefLinear, id));
}
export inline bool is_linear(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefLinear;
}
export inline std::uint64_t as_linear_id(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

export inline EvalValue make_keyword(std::uint64_t idx) noexcept {
    return EvalValue(make_ref(RefKeyword, idx));
}
export inline bool is_keyword(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefKeyword;
}
export inline std::uint64_t as_keyword_idx(const EvalValue& v) noexcept {
    return ref_index(v.val);
}

// ── Truthiness (moved to aura::compiler.evaluator_pure, Issue #146) ─────
//
// `is_truthy` is now defined in aura.compiler.evaluator_pure
// (the pure-function module). Callers that previously used
// `types::is_truthy` should use `aura::compiler::pure::is_truthy`
// (import aura.compiler.evaluator_pure). Legacy callers in
// evaluator_impl.cpp use a `using` declaration at the top of
// the file to keep the unqualified `is_truthy` call sites
// working without code churn.

// ── Formatting (debug/error output) ────────────────────
export inline std::string format_value(const EvalValue& v) {
    if (is_void(v))
        return "()";
    if (is_bool(v))
        return as_bool(v) ? "#t" : "#f";
    if (is_int(v))
        return std::to_string(as_int(v));
    if (is_float(v))
        return std::to_string(as_float(v));
    if (is_string(v))
        return "\"" + std::to_string(as_string_idx(v)) + "\"";
    if (is_pair(v))
        return "#<pair " + std::to_string(as_pair_idx(v)) + ">";
    if (is_closure(v))
        return "#<closure " + std::to_string(as_closure_id(v)) + ">";
    if (is_cell(v))
        return "#<cell " + std::to_string(as_cell_id(v)) + ">";
    if (is_vector(v))
        return "#<vector " + std::to_string(as_vector_idx(v)) + ">";
    if (is_hash(v))
        return "#<hash " + std::to_string(as_hash_idx(v)) + ">";
    if (is_primitive(v))
        return "#<primitive " + std::to_string(as_primitive_slot(v)) + ">";
    if (is_module(v))
        return "#<module " + std::to_string(as_module_idx(v)) + ">";
    if (is_keyword(v))
        return "#<keyword " + std::to_string(as_keyword_idx(v)) + ">";
    if (is_error(v))
        return "#<error " + std::to_string(as_error_idx(v)) + ">";
    if (is_linear(v))
        return "#<linear " + std::to_string(as_linear_id(v)) + ">";
    if (is_opaque(v))
        return "#<opaque " + std::to_string(as_opaque_idx(v)) + ">";
    return "#<unknown " + std::to_string(v.val) + ">";
}


export inline std::string format_value(const EvalValue& v, const std::vector<std::string>* heap) {
    if (is_void(v))
        return "()";
    if (is_bool(v))
        return as_bool(v) ? "#t" : "#f";
    if (is_string(v)) {
        if (heap) {
            auto idx = as_string_idx(v);
            if (idx < heap->size())
                return std::format("\"{}\"", (*heap)[idx]);
        }
        return std::format("<string[{}]>", as_string_idx(v));
    }
    if (is_int(v))
        return std::to_string(as_int(v));
    if (is_float(v))
        return std::to_string(as_float(v));
    if (is_pair(v))
        return std::format("<pair[{}]>", as_pair_idx(v));
    if (is_closure(v))
        return "#<procedure>";
    if (is_cell(v))
        return std::format("<cell[{}]>", as_cell_id(v));
    if (is_vector(v))
        return std::format("<vector[{}]>", as_vector_idx(v));
    if (is_hash(v))
        return std::format("<hash[{}]>", as_hash_idx(v));
    if (is_primitive(v))
        return "<primitive>";
    if (is_linear(v))
        return std::format("<linear[{}]>", as_linear_id(v));
    if (is_module(v))
        return std::format("<module[{}]>", as_module_idx(v));
    if (is_error(v))
        return std::format("<error[{}]>", as_error_idx(v));
    return "<unknown>";
}

} // namespace aura::compiler::types
