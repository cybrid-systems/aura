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

} // namespace aura::compiler::types

#endif // AURA_COMPILER_VALUE_TAGS_H
