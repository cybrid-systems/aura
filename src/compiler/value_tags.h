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
//   Ref bit layout: (pool_index << kRefPoolShift) | (type << kRefTypeShift) | kRefLowTag
//
//   Float: stored in runtime pool with FLOAT_BIAS encoding
//   String: stored in runtime pool with STRING_BIAS encoding
//
#ifndef AURA_COMPILER_VALUE_TAGS_H
#define AURA_COMPILER_VALUE_TAGS_H

#include <atomic>
#include <cstdint>

#include "core/cpp26_contract_stats.h"

namespace aura::compiler::types {

// ── Bit-layout constants (#907) ───────────────────────────────────
// Ref: (pool_index << kRefPoolShift) | (type << kRefTypeShift) | kRefLowTag
inline constexpr std::uint64_t kRefLowTag = 1ULL; // lowest bit set for Ref
inline constexpr std::uint64_t kRefTypeShift = 2; // bits 2-5: RefType (4 bits)
inline constexpr std::uint64_t kRefTypeMask = 0xFULL << kRefTypeShift;
inline constexpr std::uint64_t kRefPoolShift = 6; // bits 6+: pool index
static_assert(kRefTypeShift + 4 <= kRefPoolShift, "RefType must fit below pool index");

// Fixnum: signed integer << kFixnumShift (low bit 0).
inline constexpr std::uint64_t kFixnumShift = 1;

// Closure id high bit (IR executor: id is not a pool index).
inline constexpr std::uint64_t kClosureIdHighBit = 1ULL << 48;

// ── Special sentinels (#902): low bits == 11 (tag Special) ────────
//   #f = 3, #t = 7, void = 11  (must match lib/runtime.c)
inline constexpr std::int64_t kSpecialFalse = 3;
inline constexpr std::int64_t kSpecialTrue = 7;
inline constexpr std::int64_t kSpecialVoid = 11;

// Issue #571: unified tag classification for EvalValue v2 dispatch.
enum class EvalValueTag : std::uint8_t {
    Fixnum = 0,
    Ref = 1,
    StringV2 = 2,
    Special = 3,
    Float = 4,
    Unknown = 255,
};

// consteval low-2-bit dispatch table (bits 0-1 of the tagged int64).
// Issue #571 / #1622: primary hot-path tag class table.
inline constexpr EvalValueTag kEvalValueTagLow2Table[4] = {
    EvalValueTag::Fixnum,   // 00 — also Float when in float bias range
    EvalValueTag::Ref,      // 01
    EvalValueTag::StringV2, // 10 — range-guarded by STRING_BIAS_VAL_2
    EvalValueTag::Special,  // 11 — #f/#t/void sentinels
};

// Issue #1622: documented common tag pattern set for Agents / consteval.
inline constexpr EvalValueTag kTagPatterns[4] = {
    EvalValueTag::Fixnum,
    EvalValueTag::Ref,
    EvalValueTag::StringV2,
    EvalValueTag::Special,
};

// constexpr (not consteval-only) so runtime classify can share the table.
inline constexpr EvalValueTag eval_value_tag_low2_table(std::size_t idx) noexcept {
    return kEvalValueTagLow2Table[idx & 3u];
}

// Issue #1622: Special sentinel table (exact values).
inline constexpr EvalValueTag classify_special_sentinel(std::int64_t v) noexcept {
    if (v == kSpecialFalse || v == kSpecialTrue || v == kSpecialVoid)
        return EvalValueTag::Special;
    return EvalValueTag::Unknown;
}
inline consteval EvalValueTag classify_special_sentinel_consteval(std::int64_t v) noexcept {
    return classify_special_sentinel(v);
}

// Issue #465 / #1240: consteval encoding contracts for v2 dispatch
// (low-2 table + specials + fixnum shift). Zero-overhead compile-time
// drift detection for the 64-bit tagged EvalValue hot path.
static_assert(eval_value_tag_low2_table(0) == EvalValueTag::Fixnum,
              "dispatch table drift: low2=0 must be Fixnum");
static_assert(eval_value_tag_low2_table(1) == EvalValueTag::Ref,
              "dispatch table drift: low2=1 must be Ref");
static_assert(eval_value_tag_low2_table(2) == EvalValueTag::StringV2,
              "dispatch table drift: low2=2 must be StringV2");
static_assert(eval_value_tag_low2_table(3) == EvalValueTag::Special,
              "dispatch table drift: low2=3 must be Special");
static_assert(static_cast<std::uint8_t>(EvalValueTag::Fixnum) == 0, "Fixnum tag must be 0");
static_assert(static_cast<std::uint8_t>(EvalValueTag::Ref) == 1, "Ref tag must be 1");
static_assert(static_cast<std::uint8_t>(EvalValueTag::StringV2) == 2, "StringV2 tag must be 2");
static_assert(static_cast<std::uint8_t>(EvalValueTag::Special) == 3, "Special tag must be 3");
static_assert(static_cast<std::uint8_t>(EvalValueTag::Float) == 4, "Float tag must be 4");
static_assert(kSpecialFalse == 3 && kSpecialTrue == 7 && kSpecialVoid == 11,
              "special sentinel encoding");
static_assert(kFixnumShift == 1, "fixnum shift contract");
// Unknown = 255 must stay >= 5 (so the low-2-bit dispatch
// never accidentally classifies a value as Unknown when the
// real tag is Fixnum/Ref/StringV2/Special).
static_assert(static_cast<std::uint8_t>(EvalValueTag::Unknown) >= 5,
              "Unknown tag must be >= 5 (out of low-2-bit space)");

// Issue #571 observability (relaxed-ordering; advisory counts).
inline std::atomic<std::uint64_t> value_dispatch_hit_count{0};
inline std::atomic<std::uint64_t> value_dispatch_miss_count{0};
inline std::atomic<std::uint64_t> value_contract_violation_count{0};
inline std::atomic<std::uint64_t> v2_string_collision_attempts{0};
// Issue #686: total classify_eval_value_tag invocations.
inline std::atomic<std::uint64_t> value_classify_call_count{0};

inline void record_value_dispatch_hit() noexcept {
    value_dispatch_hit_count.fetch_add(1, std::memory_order_relaxed);
}
inline void record_value_dispatch_miss() noexcept {
    value_dispatch_miss_count.fetch_add(1, std::memory_order_relaxed);
}
inline void record_v2_string_collision_attempt() noexcept {
    v2_string_collision_attempts.fetch_add(1, std::memory_order_relaxed);
}

// ── Pool bias sentinels ───────────────────────────────────────────
inline constexpr std::int64_t FLOAT_BIAS_VAL = -10000000000000000LL;
inline constexpr std::int64_t STRING_BIAS_VAL = -9000000000000000000LL;

// ── Ref sub-type ids (bits 2-5) ──────────────────────────────────
//
// Lockstep with lib/runtime.c display helpers. Reorder only with
// care — these ids are part of the on-disk / ABI tag format.
using RefType = std::uint64_t;
inline constexpr RefType RefPair = 0;
inline constexpr RefType RefClosure = 1;
inline constexpr RefType RefCell = 2;
inline constexpr RefType RefVector = 3;
inline constexpr RefType RefHash = 4;
inline constexpr RefType RefPrimitive = 5;
inline constexpr RefType RefString = 6;
inline constexpr RefType RefModule = 7;
inline constexpr RefType RefError = 8;
inline constexpr RefType RefOpaque = 9;
inline constexpr RefType RefLinear = 10;
inline constexpr RefType RefKeyword = 11;
// Compile-time guard: the slot count is part of the ABI. If you add
// a new RefType, the 4-bit allocation is already maxed out (16 entries);
// switch to 8-bit type tag in both this header and lib/runtime.c.
static_assert(RefKeyword == 11, "RefType drift: update value_tags.h + lib/runtime.c");
// Issue #742: Ref sub-type range must fit the 4-bit field (bits 2-5).
static_assert(RefKeyword < 16, "Ref sub-types must fit in 4 bits");
static_assert(RefPair == 0 && RefClosure == 1 && RefCell == 2,
              "Ref sub-type ids 0..2 must be stable for dispatch");

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
// Related: Issue #58 (archived: git tag docs-archive-pre-2026-06)
//

// ── Tag inspection / construction (low-level) ────────────────────
//
// .cpp code that doesn't want to import the full value.ixx module
// can use these directly. value.ixx itself uses the same expressions
// inside its make_/is_/as_ helpers.
inline constexpr bool is_fixnum(std::int64_t v) noexcept {
    return (static_cast<std::uint64_t>(v) & ((1ULL << kFixnumShift) - 1)) == 0;
}
inline constexpr bool is_ref(std::int64_t v) noexcept {
    return (v & 3) == static_cast<std::int64_t>(kRefLowTag);
}
inline constexpr bool is_special(std::int64_t v) noexcept {
    return (v & 3) == 3;
}

inline constexpr std::uint64_t ref_type(std::int64_t v) noexcept {
    return (static_cast<std::uint64_t>(v) >> kRefTypeShift) & 0xF;
}
inline constexpr std::uint64_t ref_index(std::int64_t v) noexcept {
    return static_cast<std::uint64_t>(v) >> kRefPoolShift;
}
inline constexpr std::int64_t make_ref(std::uint64_t type, std::uint64_t index) noexcept {
    return static_cast<std::int64_t>((index << kRefPoolShift) | (type << kRefTypeShift) |
                                     kRefLowTag);
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
static_assert((make_string_raw_v2(0) & 3) == 2, "v2 string encoding broke tag bits (idx=0)");
static_assert((make_string_raw_v2(1) & 3) == 2, "v2 string encoding broke tag bits (idx=1)");
static_assert((make_string_raw_v2(31) & 3) == 2,
              "v2 string encoding broke tag bits (idx=31 — was RefError collision)");
static_assert((make_string_raw_v2(19) & 3) == 2,
              "v2 string encoding broke tag bits (idx=19 — was RefKeyword collision)");
static_assert((make_string_raw_v2(0xFFFFULL) & 3) == 2,
              "v2 string encoding broke tag bits (idx=0xFFFF)");

// Roundtrip check at compile time: make then decode returns the
// same idx. (Sanity guard against off-by-one in the shift.)
static_assert(string_idx_raw_v2(make_string_raw_v2(0)) == 0, "v2 roundtrip broke for idx=0");
static_assert(string_idx_raw_v2(make_string_raw_v2(42)) == 42, "v2 roundtrip broke for idx=42");
static_assert(string_idx_raw_v2(make_string_raw_v2(0xFFFFFULL)) == 0xFFFFFULL,
              "v2 roundtrip broke for idx=0xFFFFF");

// ═══════════════════════════════════════════════════════════════════
// Issue #613 — Float v2 encoding (parallel to string v2 above).
//
// The pre-#613 float encoding was `FLOAT_BIAS_VAL - idx` (idx not
// shifted). With the v2 dispatch table (#571), the resulting low-2
// bits depend on `idx % 4`:
//   idx=0 → (v&3)=0 → dispatched as Fixnum/Float by range
//   idx=1 → (v&3)=3 → dispatched as Special (and missed since
//                       the value is not 3/7/11) → Unknown
//   idx=2 → (v&3)=2 → dispatched as StringV2 (catastrophic — pool
//                       index gets used as a string index, display
//                       prints "<unknown>")
//   idx=3 → (v&3)=1 → dispatched as Ref
//
// This is the source of the bash/integ regression
// (test `float`: `(/ 7.0 2)` → "<unknown>" instead of "3.5"; 2.0
// at idx=1 misclassified as Unknown → "/ 7.0 2.0" → "division
// by zero" path because coerce_one on the idx=1 float returns 0
// after the tag check fails).
//
// Fix: mirror the v2 string encoding. Subtract `idx << 2` so the
// low 2 bits are always 0 (no collision with StringV2's
// (v&3)==2, Special's (v&3)==3, or Ref's (v&3)==1). Pool capacity
// drops from 2^62 to 2^60 (still plenty for any practical float
// pool). FLOAT_BIAS_VAL_2 = FLOAT_BIAS_VAL since -10^16 already
// has low 2 bits == 0 — no bias shift needed (unlike strings,
// where STRING_BIAS_VAL_2 = STRING_BIAS_VAL + 2 to put the tag
// in the low 2 bits).
//
// All float values now satisfy (v & 3) == 0, disjoint from:
//   Ref   (v&3)==1
//   StringV2 (v&3)==2
//   Special (v&3)==3
// The remaining collision is with fixnums (also (v&3)==0); the
// range check `v <= FLOAT_BIAS_VAL_2 && v > STRING_BIAS_VAL_2`
// disambiguates them.
inline constexpr std::int64_t FLOAT_BIAS_VAL_2 = FLOAT_BIAS_VAL; // -10^16, low-2-bits = 0

inline constexpr std::int64_t make_float_raw_v2(std::uint64_t idx) noexcept {
    return FLOAT_BIAS_VAL_2 - static_cast<std::int64_t>(idx << 2);
}
inline constexpr bool is_float_raw_v2(std::int64_t v) noexcept {
    return (v & 3) == 0 && v <= FLOAT_BIAS_VAL_2 && v > STRING_BIAS_VAL_2;
}
inline constexpr std::uint64_t float_idx_raw_v2(std::int64_t v) noexcept {
    return static_cast<std::uint64_t>(FLOAT_BIAS_VAL_2 - v) >> 2;
}

// Compile-time guard: v2 helpers must produce disjoint tags.
// If this fires, FLOAT_BIAS_VAL_2 lost its low-2-bits == 0 property
// (it has to be 0 because we want the float range to overlap
// with fixnums on (v&3)==0 — the range check disambiguates).
static_assert((make_float_raw_v2(0) & 3) == 0, "v2 float encoding broke tag bits (idx=0)");
static_assert((make_float_raw_v2(1) & 3) == 0, "v2 float encoding broke tag bits (idx=1)");
static_assert((make_float_raw_v2(2) & 3) == 0, "v2 float encoding broke tag bits (idx=2)");
static_assert((make_float_raw_v2(3) & 3) == 0, "v2 float encoding broke tag bits (idx=3)");
static_assert((make_float_raw_v2(0xFFFFULL) & 3) == 0,
              "v2 float encoding broke tag bits (idx=0xFFFF)");

// Roundtrip check at compile time: make then decode returns the
// same idx. (Sanity guard against off-by-one in the shift.)
static_assert(float_idx_raw_v2(make_float_raw_v2(0)) == 0, "v2 float roundtrip broke for idx=0");
static_assert(float_idx_raw_v2(make_float_raw_v2(42)) == 42, "v2 float roundtrip broke for idx=42");
static_assert(float_idx_raw_v2(make_float_raw_v2(0xFFFFFULL)) == 0xFFFFFULL,
              "v2 float roundtrip broke for idx=0xFFFFF");

// Disjoint-tag guard between v2 string and v2 float. A string
// and a float must never collide on (v&3). Strings: (v&3)==2;
// floats: (v&3)==0. Sanity check via the range: floats at idx N
// are always > STRING_BIAS_VAL_2 (so they can't accidentally
// be in the string range) and < FLOAT_BIAS_VAL_2 (so they can't
// be in the fixnum range above FLOAT_BIAS_VAL).
static_assert(make_float_raw_v2(0) > STRING_BIAS_VAL_2,
              "v2 float upper bound is in string range (collision)");
static_assert(make_string_raw_v2(0) <= STRING_BIAS_VAL_2,
              "v2 string lower bound is above string range (collision)");

// Issue #571 / #1622: pure constexpr classifier (no atomics) for
// compile-time dispatch validation and static_assert tables.
// Mirrors runtime classify_eval_value_tag control flow.
// Named *_consteval for historical AC naming; implemented as constexpr
// so tests can also call it at runtime with dynamic samples.
inline constexpr EvalValueTag classify_eval_value_tag_consteval(std::int64_t v) noexcept {
    const auto low2 = static_cast<std::size_t>(v & 3);
    // Prefer table for primary low2 class; refine Float/String ranges.
    // Note: fixnums use (v<<1) so some fixnums have low2==2 (e.g. 1→2);
    // those must NOT be treated as StringV2 unless in STRING_BIAS range.
    const auto primary = eval_value_tag_low2_table(low2);
    if (primary == EvalValueTag::Special)
        return classify_special_sentinel(v);
    if (primary == EvalValueTag::Ref)
        return EvalValueTag::Ref;
    if (primary == EvalValueTag::StringV2) {
        if (v <= STRING_BIAS_VAL_2)
            return EvalValueTag::StringV2;
        // low2==2 but outside string bias: fixnum path (bit0 clear).
        if (is_fixnum(v) && v > FLOAT_BIAS_VAL)
            return EvalValueTag::Fixnum;
        return EvalValueTag::Unknown;
    }
    // low2 == 0: Fixnum or Float (range disambiguation).
    if (is_float_raw_v2(v))
        return EvalValueTag::Float;
    if (is_fixnum(v) && v > FLOAT_BIAS_VAL)
        return EvalValueTag::Fixnum;
    return EvalValueTag::Unknown;
}

// Issue #1622: compile-time self-checks for consteval classifier.
static_assert(classify_eval_value_tag_consteval(0) == EvalValueTag::Fixnum,
              "#1622: fixnum 0 classifies as Fixnum");
static_assert(classify_eval_value_tag_consteval(2) == EvalValueTag::Fixnum,
              "#1622: fixnum 1 encoded as 2 classifies as Fixnum");
static_assert(classify_eval_value_tag_consteval(kSpecialFalse) == EvalValueTag::Special,
              "#1622: #f is Special");
static_assert(classify_eval_value_tag_consteval(kSpecialTrue) == EvalValueTag::Special,
              "#1622: #t is Special");
static_assert(classify_eval_value_tag_consteval(kSpecialVoid) == EvalValueTag::Special,
              "#1622: void is Special");
static_assert(classify_eval_value_tag_consteval(1) == EvalValueTag::Ref,
              "#1622: low2=1 is Ref (pool index 0 Pair)");
static_assert(kTagPatterns[0] == EvalValueTag::Fixnum && kTagPatterns[3] == EvalValueTag::Special,
              "#1622: kTagPatterns order Fixnum..Special");

// Issue #571: full tagged-value classification with v2 string
// range check + float/fixnum disambiguation. Bumps dispatch hit/
// miss counters for observability via (query:value-dispatch-stats).
//
// Order matters: fixnums use (v<<1) so val=2,6,10… have (v&3)==2
// but are NOT strings — string-v2 requires (v&3)==2 AND
// v <= STRING_BIAS_VAL_2 (the v2 range guard from #181).
//
// Floats (v2 encoding, Issue #613) have (v&3)==0 and live in
// [STRING_BIAS_VAL_2, FLOAT_BIAS_VAL_2]. They share (v&3)==0
// with fixnums but are disjoint on the range (fixnums are
// v > FLOAT_BIAS_VAL_2). The float range check uses
// is_float_raw_v2 for clarity.
//
// Issue #1622: runtime path uses low2 table first, then range refine
// (same control flow as classify_eval_value_tag_consteval).
inline EvalValueTag classify_eval_value_tag(std::int64_t v) noexcept {
    value_classify_call_count.fetch_add(1, std::memory_order_relaxed);
    aura::core::cpp26::record_hotpath_invariant_hit();
    const auto low2 = static_cast<std::size_t>(v & 3);
    const auto primary = eval_value_tag_low2_table(low2);
    if (primary == EvalValueTag::Special) {
        if (v == kSpecialFalse || v == kSpecialTrue || v == kSpecialVoid) {
            record_value_dispatch_hit();
            return EvalValueTag::Special;
        }
        record_value_dispatch_miss();
        return EvalValueTag::Unknown;
    }
    if (primary == EvalValueTag::Ref) {
        record_value_dispatch_hit();
        return EvalValueTag::Ref;
    }
    if (primary == EvalValueTag::StringV2) {
        if (v <= STRING_BIAS_VAL_2) {
            record_value_dispatch_hit();
            return EvalValueTag::StringV2;
        }
        // Fixnums can have low2==2 (v=2,6,10…); not a string collision.
        if (is_fixnum(v) && v > FLOAT_BIAS_VAL) {
            record_value_dispatch_hit();
            return EvalValueTag::Fixnum;
        }
        record_v2_string_collision_attempt();
        record_value_dispatch_miss();
        return EvalValueTag::Unknown;
    }
    // low2 == 0 → Fixnum or Float
    if (is_float_raw_v2(v)) {
        record_value_dispatch_hit();
        return EvalValueTag::Float;
    }
    if (is_fixnum(v) && v > FLOAT_BIAS_VAL) {
        record_value_dispatch_hit();
        return EvalValueTag::Fixnum;
    }
    record_value_dispatch_miss();
    return EvalValueTag::Unknown;
}

// Issue #1622: range / tag validity probe for hot paths (evaluator, shape).
[[nodiscard]] inline bool is_valid_tagged_value(std::int64_t v) noexcept {
    return classify_eval_value_tag(v) != EvalValueTag::Unknown;
}

// Issue #1622: coverage flag for schema 1622.
inline std::atomic<std::uint64_t> value_dispatch_consteval_table_wired{1};
inline std::atomic<std::uint64_t> value_dispatch_hotpath_contracts_wired{1};

} // namespace aura::compiler::types

#endif // AURA_COMPILER_VALUE_TAGS_H
