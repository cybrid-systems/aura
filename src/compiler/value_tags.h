// value_tags.h — Cross-boundary tagged-value constants for the compiler.
//
// ┌──────────────────────────────────────────────────────────────────┐
// │  MODULE BOUNDARY                                                  │
// │                                                                   │
// │  This is a traditional header, deliberately NOT a C++ module.     │
// │                                                                   │
// │  value.ixx is a `module;` and the tag/bias constants it owns      │
// │  (FLOAT_BIAS_VAL, STRING_BIAS_VAL, RefPair/RefClosure/...) are    │
// │  intentionally not `export`ed — they're implementation details   │
// │  of the tagged-value encoding shared with lib/runtime.c.          │
// │                                                                   │
// │  C++26 module rules make these constants invisible to .cpp        │
// │  translation units. Without this header, every .cpp that wants    │
// │  to inspect or produce tagged values either (a) duplicates the    │
// │  constants — drift risk, or (b) hardcodes magic numbers — bug     │
// │  risk.                                                            │
// │                                                                   │
// │  Fix: keep constants in a plain header. value.ixx and lib/runtime.│
// │  c #include this header (lib/runtime.c after a #include guard for  │
// │  C vs C++); any .cpp in the compiler can include it directly.     │
// │                                                                   │
// │  See issue #58.                                                   │
// └──────────────────────────────────────────────────────────────────┘
//
// ── Bit encoding (must match lib/runtime.c and value.ixx) ───────────
//
//   bits 0-1: TAG
//     00 = Fixnum (signed integer, value >> 1)
//     01 = Ref    (pool-indexed heap type)
//     11 = Special (#f=3, #t=7, void=11)
//
//   Ref sub-type (bits 2-5, 4 bits = 16 types):
//     0=Pair, 1=Closure, 2=Cell, 3=Vector,
//     4=Hash, 5=Primitive, 6=String, 7=Module,
//     8=Error, 9=Opaque, 10=Linear, 11=Keyword
//
//   Ref bit layout: (pool_index << 6) | (type << 2) | 1
//
//   Float: stored in runtime pool with FLOAT_BIAS encoding
//   String: stored in runtime pool with STRING_BIAS encoding
//
#ifndef AURA_COMPILER_VALUE_TAGS_H
#define AURA_COMPILER_VALUE_TAGS_H

#include <cstdint>

namespace aura::compiler::types {

// ── Pool bias sentinels ───────────────────────────────────────────
inline constexpr std::int64_t FLOAT_BIAS_VAL  = -10000000000000000LL;
inline constexpr std::int64_t STRING_BIAS_VAL = -9000000000000000000LL;

// ── Ref sub-type ids (bits 2-5) ──────────────────────────────────
//
// Lockstep with lib/runtime.c display helpers. Reorder only with
// care — these ids are part of the on-disk / ABI tag format.
using RefType = std::uint64_t;
inline constexpr RefType RefPair     = 0;
inline constexpr RefType RefClosure  = 1;
inline constexpr RefType RefCell     = 2;
inline constexpr RefType RefVector   = 3;
inline constexpr RefType RefHash     = 4;
inline constexpr RefType RefPrimitive = 5;
inline constexpr RefType RefString   = 6;
inline constexpr RefType RefModule   = 7;
inline constexpr RefType RefError    = 8;
inline constexpr RefType RefOpaque   = 9;
inline constexpr RefType RefLinear   = 10;
inline constexpr RefType RefKeyword  = 11;
// Compile-time guard: the slot count is part of the ABI. If you add
// a new RefType, the 4-bit allocation is already maxed out (16 entries);
// switch to 8-bit type tag in both this header and lib/runtime.c.
static_assert(RefKeyword == 11, "RefType drift: update value_tags.h + lib/runtime.c");

// ═══════════════════════════════════════════════════════════════════
// Cross-cutting tag map (Issue #58 documentation)
// ═══════════════════════════════════════════════════════════════════
//
// The shape system (src/compiler/shape.h) and the type system
// (src/core/type.ixx) need to map onto the runtime value tag
// (RefType below). Add a NEW entry to BOTH the ShapeTag enum (in
// shape.h) and the RefType list above, then update the table here.
//
//   ShapeTag              RefType        Notes
//   ─────────────────     ─────────      ───────────────────
//   ShapeTag::Any         DYNAMIC        (no ref encoding; type_id = 0)
//   ShapeTag::Int         (fixnum)       not a RefType; encoded in val bits
//   ShapeTag::Float       (float bias)   not a RefType; FLOAT_BIAS encoding
//   ShapeTag::Bool        (special)      not a RefType; val == 3 or 7
//   ShapeTag::String      (string bias)  not a RefType; STRING_BIAS encoding
//   ShapeTag::Void        (special)      not a RefType; val == 11
//   ShapeTag::Pair        RefPair = 0
//   ShapeTag::Vector      RefVector = 3
//   ShapeTag::Hash        RefHash = 4
//   ShapeTag::Closure     RefClosure = 1
//   ShapeTag::Ref         (any)          runtime-agnostic; subtype via type_id
//   ShapeTag::Struct      RefOpaque = 9
//   ShapeTag::Union       RefOpaque = 9  (one mapping; could be split later)
//
// The ShapeTag in shape.h is its own enum class. RefType here is
// the on-disk/in-memory tag. When a Shape is constructed (e.g. in
// shape_profiler.cpp), it must call `static_cast<std::uint8_t>(...)`
// to match RefType before storing — see shape_profiler.cpp:49-55 for
// the canonical mapping function.
//
// Related: docs/design/issue-58-module-boundaries.md
//

// ── Tag inspection / construction (low-level) ────────────────────
//
// .cpp code that doesn't want to import the full value.ixx module
// can use these directly. value.ixx itself uses the same expressions
// inside its make_/is_/as_ helpers.
inline constexpr bool is_fixnum(std::int64_t v) noexcept  { return (v & 1) == 0; }
inline constexpr bool is_ref(std::int64_t v) noexcept     { return (v & 3) == 1; }
inline constexpr bool is_special(std::int64_t v) noexcept { return (v & 3) == 3; }

inline constexpr std::uint64_t ref_type(std::int64_t v) noexcept {
    return (static_cast<std::uint64_t>(v) >> 2) & 0xF;
}
inline constexpr std::uint64_t ref_index(std::int64_t v) noexcept {
    return static_cast<std::uint64_t>(v) >> 6;
}
inline constexpr std::int64_t make_ref(std::uint64_t type, std::uint64_t index) noexcept {
    return static_cast<std::int64_t>((index << 6) | (type << 2) | 1ULL);
}

// String bias helpers — these are paired with lib/runtime.c.
inline constexpr std::int64_t make_string_raw(std::uint64_t idx) noexcept {
    return STRING_BIAS_VAL - static_cast<std::int64_t>(idx);
}
inline constexpr std::uint64_t string_idx_raw(std::int64_t v) noexcept {
    return static_cast<std::uint64_t>(-static_cast<std::int64_t>(v) - 9000000000000000000LL);
}

// ═══════════════════════════════════════════════════════════════════
// Issue #181 — EvalValue 64-bit tagged encoding redesign (Cycle 1)
//
// Option A: dedicated tag bit for strings at (v & 3) == 2.
// The current encoding (above) collides with is_ref() for odd
// indices: for idx ≡ 31 (mod 64), the result has (v & 3) == 1
// matching is_ref, and ref_type matches RefError (8) — so the
// string is misclassified as RefError. This is the source of
// the bug per the issue body.
//
// The v2 encoding shifts idx left by 2 to reserve the low 2
// bits for a constant tag. Pool capacity drops from 2^62 to
// 2^60 (still plenty for any practical string pool).
//
//   bits 0-1: TAG = 2 (string, was unused in old encoding)
//   bits 2+:   (STRING_BIAS_VAL_2 - v) >> 2  = idx
//
// All string values have (v & 3) == 2 — disjoint from fixnum
// (0), ref (1), and special (3). is_string becomes a simple
// tag check; no range disambiguation needed.
//
// Migration is the work of Cycle 2. Cycle 1 only prototypes
// these helpers alongside the old encoding so the new design
// can be verified in isolation (exhaustive collision tests +
// micro-benchmarks).
// ═══════════════════════════════════════════════════════════════════

// New bias for the v2 encoding: same magnitude as STRING_BIAS_VAL
// but with low 2 bits = 2 (the string tag).
//   STRING_BIAS_VAL_2 = STRING_BIAS_VAL + 2  (low 2 bits: 0 → 2)
inline constexpr std::int64_t STRING_BIAS_VAL_2 = STRING_BIAS_VAL + 2;

inline constexpr std::int64_t make_string_raw_v2(std::uint64_t idx) noexcept {
    return STRING_BIAS_VAL_2 - static_cast<std::int64_t>(idx << 2);
}
inline constexpr bool is_string_raw_v2(std::int64_t v) noexcept {
    return (v & 3) == 2;
}
inline constexpr std::uint64_t string_idx_raw_v2(std::int64_t v) noexcept {
    return static_cast<std::uint64_t>(STRING_BIAS_VAL_2 - v) >> 2;
}

// Compile-time guard: v2 helpers must produce disjoint tags.
// (If this fires, the encoding was changed without updating
// the tag bits — fix the constants above.)
static_assert((make_string_raw_v2(0) & 3) == 2,
              "v2 string encoding broke tag bits (idx=0)");
static_assert((make_string_raw_v2(1) & 3) == 2,
              "v2 string encoding broke tag bits (idx=1)");
static_assert((make_string_raw_v2(31) & 3) == 2,
              "v2 string encoding broke tag bits (idx=31 — was RefError collision)");
static_assert((make_string_raw_v2(19) & 3) == 2,
              "v2 string encoding broke tag bits (idx=19 — was RefKeyword collision)");
static_assert((make_string_raw_v2(0xFFFFULL) & 3) == 2,
              "v2 string encoding broke tag bits (idx=0xFFFF)");

// Roundtrip check at compile time: make then decode returns the
// same idx. (Sanity guard against off-by-one in the shift.)
static_assert(string_idx_raw_v2(make_string_raw_v2(0)) == 0,
              "v2 roundtrip broke for idx=0");
static_assert(string_idx_raw_v2(make_string_raw_v2(42)) == 42,
              "v2 roundtrip broke for idx=42");
static_assert(string_idx_raw_v2(make_string_raw_v2(0xFFFFFULL)) == 0xFFFFFULL,
              "v2 roundtrip broke for idx=0xFFFFF");

} // namespace aura::compiler::types

#endif // AURA_COMPILER_VALUE_TAGS_H
