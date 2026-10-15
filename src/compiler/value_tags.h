// value_tags.h — Cross-boundary tagged-value constants for the compiler.
//
// ┌──────────────────────────────────────────────────────────────────┐
// │  MODULE BOUNDARY                                                  │
// │                                                                   │
// │  This is a traditional header, deliberately NOT a C++ module.     │
// │                                                                   │
// │  value.ixx is a `module;` and the tag/bias constants it owns      │
// │  (kSpecial*, RefPair/RefClosure/..., boxed float/string helpers)  │
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
//   Source of truth: lib/aura_value_abi.h (ABI 3, #4482 / #4502).
//   bit 0 == 0: Fixnum (signed, value >> 1, symmetric [-2^62, 2^62-1])
//   bits 0-1 == 01: Ref (pool-indexed heap type)
//   bits 0-1 == 11: Immediate, v = (payload << 8) | (kind << 2) | 3
//     kind 0/1/2 = #f/#t/void (raw 3/7/11), 8 = Float, 9 = String
//
//   Ref sub-type (bits 2-5, 4 bits = 16 types):
//     0=Pair, 1=Closure, 2=Cell, 3=Vector,
//     4=Hash, 5=Primitive, 6=String, 7=Module,
//     8=Error, 9=Opaque, 10=Linear, 11=Keyword
//
//   Ref bit layout: (pool_index << kRefPoolShift) | (type << kRefTypeShift) | kRefLowTag
//
//   Float / String: ImmKind 8 / 9, payload = runtime pool index
//
#ifndef AURA_COMPILER_VALUE_TAGS_H
#define AURA_COMPILER_VALUE_TAGS_H

#include <atomic>
#include <cstdint>

#include "core/cpp26_contract_stats.h"
// #4502 step 1: C ABI header is the single source of truth.
#include "../../lib/aura_value_abi.h"

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
inline constexpr std::uint64_t kClosureIdHighBit = AURA_CLOSURE_ID_HIGH_BIT;
// #4502 step 6: closure ids are a field, not a magnitude. Bit 48 is the
// "IR closure id" namespace flag; bits [0, 48) are the id within it.
inline constexpr std::uint64_t kClosureIdSpaceMask = kClosureIdHighBit - 1;
[[nodiscard]] inline constexpr bool is_ir_closure_id(std::uint64_t cid) noexcept {
    return (cid & kClosureIdHighBit) != 0;
}
[[nodiscard]] inline constexpr std::uint64_t make_ir_closure_id(std::uint64_t n) noexcept {
    return kClosureIdHighBit | (n & kClosureIdSpaceMask);
}
[[nodiscard]] inline constexpr std::uint64_t ir_closure_id_index(std::uint64_t cid) noexcept {
    return cid & kClosureIdSpaceMask;
}
static_assert(is_ir_closure_id(make_ir_closure_id(0)) && !is_ir_closure_id(kClosureIdSpaceMask),
              "#4502: closure id namespace bit");

// ── Special sentinels (#902): low bits == 11 (tag Special) ────────
//   #f = 3, #t = 7, void = 11  (must match lib/runtime.c)
inline constexpr std::int64_t kSpecialFalse = AURA_V_FALSE;
inline constexpr std::int64_t kSpecialTrue = AURA_V_TRUE;
inline constexpr std::int64_t kSpecialVoid = AURA_V_VOID;
// #4502 step 4: unspecified value of (if #f #f) / when / unless; not '().
inline constexpr std::int64_t kSpecialUnspecified = AURA_V_UNSPEC;

// Issue #571: unified tag classification for EvalValue v2 dispatch.
enum class EvalValueTag : std::uint8_t {
    Fixnum = 0,
    Ref = 1,
    StringV2 = 2, // enum id only; the string encoding is ImmKind 9 (#4502)
    Special = 3,
    Float = 4,
    Unknown = 255,
};

// consteval low-2-bit dispatch table (bits 0-1 of the tagged int64).
// Issue #571 / #1622: primary hot-path tag class table.
inline constexpr EvalValueTag kEvalValueTagLow2Table[4] = {
    EvalValueTag::Fixnum,  // 00 — also Float when in float bias range
    EvalValueTag::Ref,     // 01
    EvalValueTag::Fixnum,  // 10 — odd fixnums (#4482: always Fixnum)
    EvalValueTag::Special, // 11 — sentinels; Float/String by range (#4482)
};

// Issue #1622: documented common tag pattern set for Agents / consteval.
inline constexpr EvalValueTag kTagPatterns[4] = {
    EvalValueTag::Fixnum,
    EvalValueTag::Ref,
    EvalValueTag::Fixnum,
    EvalValueTag::Special,
};

// constexpr (not consteval-only) so runtime classify can share the table.
inline constexpr EvalValueTag eval_value_tag_low2_table(std::size_t idx) noexcept {
    return kEvalValueTagLow2Table[idx & 3u];
}

// Issue #1622: Special sentinel table (exact values).
inline constexpr EvalValueTag classify_special_sentinel(std::int64_t v) noexcept {
    if (v == kSpecialFalse || v == kSpecialTrue || v == kSpecialVoid || v == kSpecialUnspecified)
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
static_assert(eval_value_tag_low2_table(2) == EvalValueTag::Fixnum,
              "dispatch table drift: low2=2 must be Fixnum (#4482)");
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

// ── Truthiness (Issue #4491) ─────────────────────────────────────
// Single source of truth for whether fixnum 0 is false. Every
// truthiness test (pure::is_truthy, JIT jit_truthy, constant folding,
// type checker) reads this flag so the paths cannot drift apart.
// Issue #4491: only #f is false (R7RS 6.3). Integer 0 is true.
inline constexpr bool kIntZeroIsFalse = false;
[[nodiscard]] inline constexpr bool raw_is_truthy(std::int64_t v) noexcept {
    if (v == kSpecialFalse)
        return false;
    if constexpr (kIntZeroIsFalse)
        return v != 0;
    return true;
}

// ── Immediate kinds (#4502 step 5) ────────────────────────────────
// low2 == 11 values are immediates: v = (payload << 8) | (kind << 2) | 3
// (lib/aura_value_abi.h). #f/#t/void are kinds 0/1/2 with payload 0
// (raw 3/7/11); boxed floats and strings are kinds 8/9 with the pool
// index as payload. Classification is a low-byte compare, never a range.
inline constexpr std::int64_t kImmKindFloat = AURA_KIND_FLOAT;
inline constexpr std::int64_t kImmKindString = AURA_KIND_STRING;
inline constexpr std::int64_t kFloatLow8 = AURA_FLOAT_LOW8;
inline constexpr std::int64_t kStringLow8 = AURA_STRING_LOW8;
static_assert(AURA_IMM_LOW8(AURA_KIND_FLOAT) == AURA_FLOAT_LOW8, "#4502: float low byte drift");
static_assert(AURA_IMM_LOW8(AURA_KIND_STRING) == AURA_STRING_LOW8, "#4502: string low byte drift");
static_assert(AURA_IMM(AURA_KIND_FALSE, 0) == AURA_V_FALSE &&
                  AURA_IMM(AURA_KIND_TRUE, 0) == AURA_V_TRUE &&
                  AURA_IMM(AURA_KIND_VOID, 0) == AURA_V_VOID,
              "#4502: specials are ImmKind 0/1/2 with payload 0");

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
//   ShapeTag::Float       (ImmKind 8)    not a RefType; boxed pool index
//   ShapeTag::Bool        (special)      not a RefType; val == 3 or 7
//   ShapeTag::String      (ImmKind 9)    not a RefType; boxed pool index
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
    return AURA_IS_SPECIAL(v); // #4502: exact sentinels; agrees with classify
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

// Boxed string / float helpers (#181 / #613 history; #4502 step 5 layout).
// Value encoding ABI version, mirrored by lib/runtime.c and the AOT preamble.
inline constexpr int kValueEncodingAbi = AURA_VALUE_ABI;
static_assert(kValueEncodingAbi == 4, "#4502: value_tags.h is written for ABI 4");

inline constexpr std::int64_t make_string_raw_v2(std::uint64_t idx) noexcept {
    return AURA_MAKE_STRING(idx);
}
inline constexpr bool is_string_raw_v2(std::int64_t v) noexcept {
    return AURA_IS_STRING(v);
}
inline constexpr std::uint64_t string_idx_raw_v2(std::int64_t v) noexcept {
    return AURA_IMM_PAYLOAD(v);
}
inline constexpr std::int64_t make_float_raw_v2(std::uint64_t idx) noexcept {
    return AURA_MAKE_FLOAT(idx);
}
inline constexpr bool is_float_raw_v2(std::int64_t v) noexcept {
    return AURA_IS_FLOAT(v);
}
inline constexpr std::uint64_t float_idx_raw_v2(std::int64_t v) noexcept {
    return AURA_IMM_PAYLOAD(v);
}

// Tag and roundtrip guards, including indices up to 2^56 (AC: no type change).
static_assert(is_string_raw_v2(make_string_raw_v2(0)) && is_string_raw_v2(make_string_raw_v2(31)),
              "string tag");
static_assert(is_float_raw_v2(make_float_raw_v2(0)) && is_float_raw_v2(make_float_raw_v2(3)),
              "float tag");
static_assert(!is_float_raw_v2(make_string_raw_v2(7)) && !is_string_raw_v2(make_float_raw_v2(7)),
              "float/string disjoint");
static_assert(string_idx_raw_v2(make_string_raw_v2(42)) == 42, "string roundtrip");
static_assert(float_idx_raw_v2(make_float_raw_v2(0xFFFFFULL)) == 0xFFFFFULL, "float roundtrip");
static_assert(float_idx_raw_v2(make_float_raw_v2((1ULL << 56) - 1)) == (1ULL << 56) - 1 &&
                  is_float_raw_v2(make_float_raw_v2((1ULL << 56) - 1)),
              "float index up to 2^56 keeps its type");
static_assert(string_idx_raw_v2(make_string_raw_v2((1ULL << 56) - 1)) == (1ULL << 56) - 1 &&
                  is_string_raw_v2(make_string_raw_v2((1ULL << 56) - 1)),
              "string index up to 2^56 keeps its type");

// Issue #571 / #1622: pure constexpr classifier (no atomics) for
// compile-time dispatch validation and static_assert tables.
// Mirrors runtime classify_eval_value_tag control flow.
// Named *_consteval for historical AC naming; implemented as constexpr
// so tests can also call it at runtime with dynamic samples.
inline constexpr EvalValueTag classify_eval_value_tag_consteval(std::int64_t v) noexcept {
    // Issue #4482: bit0 clear is always Fixnum (full symmetric range).
    // #4502: low2 == 11 is an immediate; its low byte names the kind.
    if (is_fixnum(v))
        return EvalValueTag::Fixnum;
    if ((v & 3) == 1)
        return EvalValueTag::Ref;
    if (is_string_raw_v2(v))
        return EvalValueTag::StringV2;
    if (is_float_raw_v2(v))
        return EvalValueTag::Float;
    return classify_special_sentinel(v); // unassigned kinds -> Unknown
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

// Issue #571 / #2616: full tagged-value classification with v2 string
// range check + float/fixnum disambiguation. Bumps dispatch hit/
// miss counters for observability via (query:value-dispatch-stats).
//
// *** HOT-PATH BAN (#2616 / #2259) ***
// Do NOT call this from eval_flat / ir_executor / apply tight loops —
// every invoke does atomic RMW on value_classify_call_count. Use
// is_fixnum_hot / is_string_v2_hot / is_* / as_* (value.ixx) or
// classify_eval_value_tag_consteval instead. Gate:
// scripts/coverage/checks/check_value_tag_hotpath_ban_2616.py.
//
// #4502: fixnum is bit0 == 0; float and string are exact low-byte kinds.
//
// Issue #1622: runtime path uses low2 table first, then the kind compare
// (same control flow as classify_eval_value_tag_consteval).
// Cold / Agent only — intentional atomic observability.
inline EvalValueTag classify_eval_value_tag(std::int64_t v) noexcept {
    value_classify_call_count.fetch_add(1, std::memory_order_relaxed);
    aura::core::cpp26::record_hotpath_invariant_hit();
    const auto tag = classify_eval_value_tag_consteval(v);
    if (tag == EvalValueTag::Unknown)
        record_value_dispatch_miss();
    else
        record_value_dispatch_hit();
    return tag;
}

// Issue #1622 / #2616: pure range/tag validity for hot paths (no atomics).
// Agent full classification with counters: call classify_eval_value_tag.
[[nodiscard]] inline constexpr bool is_valid_tagged_value(std::int64_t v) noexcept {
    return classify_eval_value_tag_consteval(v) != EvalValueTag::Unknown;
}
// Alias for explicit hot-path call sites / gate documentation.
[[nodiscard]] inline constexpr bool is_valid_tagged_value_hot(std::int64_t v) noexcept {
    return is_valid_tagged_value(v);
}

// ── Issue #2259 / #2616: zero-overhead pure tag tests (no atomics) ──
//
// classify_eval_value_tag() bumps process-wide counters — correct for
// Agent dashboards, **hard-banned** on eval_flat / IR arithmetic / apply
// tight loops (#2616). These pure helpers share the same control flow as
// classify_eval_value_tag_consteval (single low2 branch + range refine)
// with zero atomic cost under NDEBUG contracts.
//
// Prefer these (or is_*/as_* in value.ixx which route here) on
// eval_flat / call-apply / ir_executor tight loops.
[[nodiscard]] inline constexpr bool is_fixnum_hot(std::int64_t v) noexcept {
    // #4524: fixnum-first single-branch form (bit0 == 0, AURA_IS_FIXNUM).
    // Identical to the classifier's first test (#4482: bit0 clear is always
    // Fixnum) — one predictor input on the eval_flat / IR / apply loops
    // instead of the full classify walk (#2259 style). The static_asserts
    // below pin the equivalence.
    return (static_cast<std::uint64_t>(v) & 1ULL) == 0;
}
[[nodiscard]] inline constexpr bool is_ref_hot(std::int64_t v) noexcept {
    return classify_eval_value_tag_consteval(v) == EvalValueTag::Ref;
}
[[nodiscard]] inline constexpr bool is_string_v2_hot(std::int64_t v) noexcept {
    return classify_eval_value_tag_consteval(v) == EvalValueTag::StringV2;
}
[[nodiscard]] inline constexpr bool is_special_hot(std::int64_t v) noexcept {
    return classify_eval_value_tag_consteval(v) == EvalValueTag::Special;
}
[[nodiscard]] inline constexpr bool is_float_hot(std::int64_t v) noexcept {
    // #4524: float second — direct low8 ImmKind compare (AURA_IS_FLOAT,
    // #4502) instead of walking the classify chain's fixnum/ref/string
    // probes first. Disjoint by construction: the float low byte is 35
    // (odd → not fixnum, low2=11 → not ref, != 39 → not string), so the
    // single-branch form classifies exactly the values the chain calls
    // Float. Equivalence pinned by the static_asserts below.
    return (v & 0xFF) == kFloatLow8;
}

// #4524: single-branch hot forms stay equivalent to the cold classifier
// over every tag class (fixnum / ref / special / float / string encodings).
static_assert(is_fixnum_hot(0) && is_fixnum_hot(2) && is_fixnum_hot(-4));
static_assert(!is_fixnum_hot(1) && !is_fixnum_hot(kSpecialFalse) && !is_fixnum_hot(kSpecialVoid));
static_assert(is_float_hot(make_float_raw_v2(0)) && is_float_hot(make_float_raw_v2(999999)));
static_assert(!is_float_hot(make_string_raw_v2(0)) && !is_float_hot(0) && !is_float_hot(1));
static_assert(!is_float_hot(kSpecialTrue) && !is_float_hot(kSpecialUnspecified));
// Single low2 extract — one branch predictor input for tagged dispatch.
[[nodiscard]] inline constexpr std::uint8_t tag_low2_hot(std::int64_t v) noexcept {
    return static_cast<std::uint8_t>(v & 3);
}

// AC4 metrics: hot-path untag / tag-test samples (as_* / explicit note).
inline std::atomic<std::uint64_t> value_tag_hot_path_total{0};
// Debug-only contract fails (AURA_CONTRACTS_OBSERVE / enforce path mirrors).
inline std::atomic<std::uint64_t> value_tag_hot_contract_fail_total{0};
// Tag stability feed for ShapeProfiler (#2259 goal 4).
inline std::atomic<std::uint64_t> value_tag_stability_fixnum_total{0};
inline std::atomic<std::uint64_t> value_tag_stability_ref_total{0};
inline std::atomic<std::uint64_t> value_tag_stability_run_total{0};
inline std::atomic<std::uint64_t> value_tag_hotpath_zero_overhead_wired{1};

inline void note_value_tag_hot_path() noexcept {
#ifdef AURA_CONTRACTS_OBSERVE
    // Issue #3391 (I1): hot-path note must be zero-overhead under NDEBUG.
    // Soft / AURA_CONTRACTS_OBSERVE builds keep the process atomic for
    // Agent-visible counters; production / NDEBUG emits only the check
    // (the fetch_add is compiled out). #2616 already banned classify
    // atomics on eval/IR/apply hot paths — this closes the residual
    // atomic on the *allowed* as_* surface.
    value_tag_hot_path_total.fetch_add(1, std::memory_order_relaxed);
#endif
}

inline void note_value_tag_hot_contract_fail() noexcept {
    value_tag_hot_contract_fail_total.fetch_add(1, std::memory_order_relaxed);
    value_contract_violation_count.fetch_add(1, std::memory_order_relaxed);
    aura::core::cpp26::record_contract_violation_hotpath();
}

// ShapeProfiler / speculative confidence: consecutive Fixnum or Ref
// samples raise stability counters (fiber-local run length).
inline void note_value_tag_stability(EvalValueTag tag) noexcept {
    // Thread-local run tracking — no heap; cheap for multi-round AI mutate.
    thread_local EvalValueTag prev = EvalValueTag::Unknown;
    thread_local std::uint32_t run = 0;
    if (tag == prev && (tag == EvalValueTag::Fixnum || tag == EvalValueTag::Ref)) {
        ++run;
        if (run >= 2) {
            value_tag_stability_run_total.fetch_add(1, std::memory_order_relaxed);
            if (tag == EvalValueTag::Fixnum)
                value_tag_stability_fixnum_total.fetch_add(1, std::memory_order_relaxed);
            else
                value_tag_stability_ref_total.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        prev = tag;
        run = 1;
    }
}

// Issue #1622: coverage flag for schema 1622.
inline std::atomic<std::uint64_t> value_dispatch_consteval_table_wired{1};
inline std::atomic<std::uint64_t> value_dispatch_hotpath_contracts_wired{1};
// Issue #2259: pure hot-path tag tests + as_* contracts.
inline std::atomic<std::uint64_t> value_tag_hotpath_2259_wired{1};
// Issue #2616: hard-ban classify_eval_value_tag on production hot TUs (gate).
inline std::atomic<std::uint64_t> value_tag_hotpath_ban_2616_wired{1};
inline constexpr int kValueTagHotpathBanIssue = 2616;

} // namespace aura::compiler::types

#endif // AURA_COMPILER_VALUE_TAGS_H
