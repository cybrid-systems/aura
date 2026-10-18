// Issue #146 — pure-function + monadic Result extraction for the
// Evaluator core. This module houses stateless computational
// kernels that operate only on their arguments (no `this`,
// no Evaluator member access) and return `aura::compiler::Result`
// (`std::expected<T, Diagnostic>`) instead of bool+string or
// ad-hoc std::optional.
//
// Pattern (Issue #126 + ComputeKindWrap precedent): legacy
// stateful call sites can keep using the old non-Result API
// (a thin in-source wrapper that calls the pure version with
// `.value_or(default)`), while new code and tests can use the
// pure Result-returning version directly for monadic composition
// (`.and_then`, `.or_else`, `?`).
//
// The pure functions have NO access to:
//   - Evaluator member state (cells_, pairs_, string_heap_, ...)
//   - Module-level globals
//   - Mutable state across calls
// All dependencies are passed in as arguments (typically the
// relevant span/slice, e.g. the string heap for coercion).
//
// Phase 1 (this commit): extract `coerce_to_int` as
// `coerce_to_int_pure`. Subsequent PRs will extract the
// remaining 5-7 functions to hit the #146 acceptance criteria
// (6-8 pure functions total).

module;
#include "compiler/locked_string_heap.hh" // Issue #4543
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <memory_resource>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include "value_tags.h"
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>

export module aura.compiler.evaluator_pure;

import std;
import aura.core;
import aura.diag;
import aura.compiler.value;
import aura.core.type;

namespace aura::compiler::pure {

// is_truthy — Issue #146 Phase 3 extract.
//
// Pure truthiness test (no this-> access, all inputs as
// parameters). Was previously in aura::compiler::types
// (value.ixx / value_impl.cpp). Now canonical home is
// aura::compiler::pure for #146's pure-function module.
// The legacy types::is_truthy remains as a using-alias for
// backward compat — existing callers continue to work.
//
// Semantics: VOID/FALSE/0 → false; everything else → true.
// The implementation is intentionally a no-op for the
// common case (bool / int) to keep the hot path branch-free
// at the cost of an int comparison. Matches legacy.
inline void note_int_zero_truth_test() noexcept {
    static const bool enabled = [] {
        const char* e = std::getenv("AURA_TRUTHY_WARN");
        return e != nullptr && e[0] == '1';
    }();
    static std::atomic<bool> warned{false};
    if (enabled && !warned.exchange(true))
        std::println(std::cerr, "aura: warning: condition tested integer 0; since #4491 only #f "
                                "is false and 0 is true (AURA_TRUTHY_WARN)");
}
export inline bool is_truthy(const types::EvalValue& v) noexcept {
    // Issue #4491: shared with JIT / constant folding via value_tags.h.
    // AURA_TRUTHY_WARN=1 reports (once) a test on integer 0, which was
    // false before #4491 and is true now. Migration aid only.
    if (v.val == 0) [[unlikely]]
        note_int_zero_truth_test();
    return ::aura::compiler::types::raw_is_truthy(v.val);
}

// edit_distance_pure — Issue #146 Phase 4 extract.
//
// Pure Levenshtein distance (insertion/deletion/substitution
// counts, all weight 1). Two-row DP for O(m+n) memory
// (instead of the O(m*n) full table). No Evaluator state
// access; all inputs are string_view spans.
export inline std::size_t edit_distance_pure(std::string_view a, std::string_view b) {
    auto m = a.size();
    auto n = b.size();
    if (m == 0)
        return n;
    if (n == 0)
        return m;
    // Two-row DP — swap prev/cur each iteration.
    std::vector<std::size_t> prev(n + 1);
    std::vector<std::size_t> cur(n + 1);
    for (std::size_t j = 0; j <= n; ++j)
        prev[j] = j;
    for (std::size_t i = 1; i <= m; ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= n; ++j) {
            auto cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[n];
}

// closest_match_pure — Issue #146 Phase 4 extract.
//
// Pure "did you mean X?" helper. Finds the candidate in
// `candidates` with the smallest edit distance to `name`.
// Returns the best match if its distance is within `max_dist`
// (default 3), or empty string if no candidate is close
// enough. The empty-string sentinel matches the legacy
// "no match" convention.
export inline std::string closest_match_pure(std::string_view name,
                                             std::span<const std::string> candidates,
                                             std::size_t max_dist = 3) {
    std::string best;
    std::size_t best_dist = max_dist + 1;
    for (auto& c : candidates) {
        auto d = edit_distance_pure(name, c);
        if (d < best_dist) {
            best_dist = d;
            best = c;
        }
    }
    return best;
}

// ── FFIRuntime marshalling (Issue #146 Phase 5 extract) ────────
//
// The FFI closure-dispatch path in Evaluator::apply_closure
// needs to marshal EvalValue args into the raw C ABI
// (i64 / f64 / char* / void*) that foreign functions expect.
// The marshalling logic is a mechanical transformation
// that has no Evaluator state access — all dependencies
// (args, arg_types, string_heap, opaque_heap) are
// parameters. Pulling it into a pure function makes it
// unit-testable in isolation (no Evaluator plumbing) and
// unblocks monadic composition for new FFI call sites.

// Tagged arg type codes — match the int constants used
// elsewhere in FFIRuntime for the arg_types array.
//   1 = Int, 2 = Float, 3 = String, 4 = Opaque, 5 = Handle:<tag>
//
// Issue #4553: slot limits. The old marshaler stopped at 6 arguments in
// total and silently dropped the rest. ffi_call_by_class now passes up to
// kFfiMaxIntSlots integer-class arguments (Int / String / Opaque; past
// the register count they go to the stack in declared order) and up to
// kFfiMaxSseSlots Float arguments (all in registers, so no Float ever
// spills and the int-first prototype stays ABI-compatible). c-func
// rejects a signature outside these limits when it is created
// (ffi_primitives.ixx kFfiSigMaxIntSlots / kFfiSigMaxSseSlots mirror them).
export inline constexpr std::size_t kFfiMaxIntSlots = 16;
export inline constexpr std::size_t kFfiMaxSseSlots = 8;
export inline constexpr std::size_t kFfiMaxArgs = kFfiMaxIntSlots + kFfiMaxSseSlots;

struct FFIMarshalled {
    // Per declared argument (indexed like args / arg_types).
    std::array<std::int64_t, kFfiMaxArgs> i_vals{}; // raw i64 value
    std::array<double, kFfiMaxArgs> d_vals{};       // raw f64 value
    std::array<const char*, kFfiMaxArgs> s_vals{};  // char* (heap bytes or sso_bufs)
    // Issue #4555: String arguments are passed zero-copy — s_vals points at
    // the string heap slot's own buffer, so the call costs the same for 16 B
    // and 1 MB. Only a string held in its SSO buffer (bytes inside the
    // std::string object, which move when another fiber's mint reallocates
    // the heap vector) is copied, into this fixed per-argument slot. That
    // keeps #4554 fixed: no slot ever moves while the call runs.
    static constexpr std::size_t kSsoBuf = 24;
    std::array<std::array<char, kSsoBuf>, kFfiMaxArgs> sso_bufs;
    // Issue #4531 / #4553: per class, in declared order — what the call
    // passes (integer registers then stack; xmm registers).
    std::array<std::int64_t, kFfiMaxIntSlots> ic{};
    std::array<double, kFfiMaxSseSlots> dc{};
    int ni = 0;
    int nd = 0;
    bool any_float = false;
    // Issue #4551: the first argument whose tag does not match its
    // declared kind (-1: none) and that kind. arity_mismatch: the call
    // passed a different number of arguments than the signature declares,
    // or the signature does not fit the slot limits.
    int bad_arg = -1;
    int bad_kind = 0;
    bool arity_mismatch = false;
    // Issue #4597: an Opaque whose generation does not match the slot, and
    // the tag a Handle:<tag> argument required.
    bool bad_stale = false;
    std::string bad_tag;
    [[nodiscard]] bool ok() const noexcept { return bad_arg < 0 && !arity_mismatch; }
};

// Issue #4551: names used in the c-func argument type error.
export inline const char* ffi_kind_name(int kind) noexcept {
    switch (kind) {
        case 2:
            return "Float";
        case 3:
            return "String";
        case 4:
            return "Opaque";
        case 5:
            return "Handle";
        default:
            return "Int";
    }
}

export inline const char* ffi_value_kind_name(const types::EvalValue& v) noexcept {
    if (types::is_int(v))
        return "Int";
    if (types::is_float(v))
        return "Float";
    if (types::is_opaque(v))
        return "Opaque";
    if (types::is_string(v))
        return "String";
    if (types::is_bool(v))
        return "Bool";
    if (types::is_void(v))
        return "Void";
    if (types::is_pair(v))
        return "Pair";
    if (types::is_vector(v))
        return "Vector";
    if (types::is_closure(v))
        return "Closure";
    if (types::is_error(v))
        return "Error";
    return "Value";
}

// ffi_marshal_args_pure — marshals EvalValue args into the raw C ABI for
// a foreign-function call. `s_vals` points into the string heap (or the
// result's `sso_bufs` for short strings); the caller keeps the result and
// the heap slots alive for the duration of the FFI call.
//
// arg_type codes: 1 = Int (also any unknown code), 2 = Float,
// 3 = String (looks up idx in string_heap, passes its bytes in place;
// SSO strings copied into sso_bufs — #4555),
// 4 = Opaque (looks up the slot in opaque_heap, passes the void*).
// 5 = Handle:<tag> (a live native handle whose tag matches).
//
// Issue #4551: each argument is checked against its declared kind
// before anything is passed: Int takes an Int; Float takes a Float or
// an Int (widened); String takes a String; Opaque takes an Opaque or an
// Int address. On a mismatch bad_arg / bad_kind name the first bad
// argument and nothing should be called (the caller raises a catchable
// type error). The argument count must equal the declared count.
//
// All inputs are read-only spans. No `this` access.
//
// Issue #4597: opaque_generation, when non-empty, rejects an Opaque whose
// generation does not match (bad_stale) instead of passing the new tenant.
// An empty span skips that check so unit tests with no generation table
// keep today's behavior. The slot is always the low 32 bits. A slot past
// the heap still passes 0. handle_tags is parallel to arg_types for kind 5.
// handle_probe->match reports whether a packed index is a live handle of
// that tag and writes its pointer.
export struct FfiHandleProbe {
    void* ctx = nullptr;
    bool (*match)(void* ctx, std::uint64_t packed, std::string_view tag, void*& ptr) = nullptr;
};

export inline FFIMarshalled
ffi_marshal_args_pure(std::span<const types::EvalValue> args, std::span<const int> arg_types,
                      std::span<const std::string> string_heap, std::span<void* const> opaque_heap,
                      std::span<const std::uint32_t> opaque_generation = {},
                      std::span<const std::string> handle_tags = {},
                      FfiHandleProbe handle_probe = {}) {
    FFIMarshalled out{};
    if (args.size() != arg_types.size() || args.size() > kFfiMaxArgs) {
        out.arity_mismatch = true;
        return out;
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        const int atype = arg_types[i];
        const auto& a = args[i];
        if (atype == 2) { // Float
            if (out.nd >= static_cast<int>(kFfiMaxSseSlots)) {
                out.arity_mismatch = true;
                return out;
            }
            if (types::is_float(a)) {
                out.d_vals[i] = types::as_float(a);
            } else if (types::is_int(a)) {
                out.d_vals[i] = static_cast<double>(types::as_int(a));
            } else {
                out.bad_arg = static_cast<int>(i);
                out.bad_kind = atype;
                return out;
            }
            out.i_vals[i] = static_cast<std::int64_t>(out.d_vals[i]);
            out.any_float = true;
            out.dc[static_cast<std::size_t>(out.nd++)] = out.d_vals[i];
            continue;
        }
        if (out.ni >= static_cast<int>(kFfiMaxIntSlots)) {
            out.arity_mismatch = true;
            return out;
        }
        if (atype == 3) { // String → char*
            if (!types::is_string(a) || types::as_string_idx(a) >= string_heap.size()) {
                out.bad_arg = static_cast<int>(i);
                out.bad_kind = atype;
                return out;
            }
            // Issue #4555: zero-copy. A heap-allocated buffer does not move
            // when the heap vector reallocates (std::string move keeps it),
            // and the argument keeps the slot live for the call (GC
            // compaction waits for this fiber's safepoint). An SSO string's
            // bytes live inside the object and move with it: copy those.
            const std::string& hs = string_heap[types::as_string_idx(a)];
            const char* d = hs.c_str();
            const auto* obj = reinterpret_cast<const char*>(&hs);
            if (d >= obj && d < obj + sizeof(std::string)) {
                auto& b = out.sso_bufs[i];
                const auto n =
                    hs.size() < FFIMarshalled::kSsoBuf - 1 ? hs.size() : FFIMarshalled::kSsoBuf - 1;
                std::memcpy(b.data(), d, n);
                b[n] = '\0';
                d = b.data();
            }
            out.s_vals[i] = d;
            out.i_vals[i] = reinterpret_cast<std::int64_t>(d);
        } else if (atype == 4) { // Opaque (void*)
            if (types::is_opaque(a)) {
                const auto packed = types::as_opaque_idx(a);
                const auto slot = types::opaque_slot_index(packed);
                const auto gen = types::opaque_generation_of(packed);
                // Slot past the heap stays a 0 pointer (today's out-of-range).
                // A generation mismatch on a live slot must not pass the new
                // tenant. An empty generation span skips the check.
                if (!opaque_generation.empty() && slot < opaque_heap.size() &&
                    (slot >= opaque_generation.size() || opaque_generation[slot] != gen)) {
                    out.bad_arg = static_cast<int>(i);
                    out.bad_kind = atype;
                    out.bad_stale = true;
                    return out;
                }
                out.i_vals[i] = slot < opaque_heap.size()
                                    ? reinterpret_cast<std::int64_t>(opaque_heap[slot])
                                    : 0;
            } else if (types::is_int(a)) {
                out.i_vals[i] = types::as_int(a);
            } else {
                out.bad_arg = static_cast<int>(i);
                out.bad_kind = atype;
                return out;
            }
        } else if (atype == 5) { // Handle:<tag>
            const std::string tag = i < handle_tags.size() ? handle_tags[i] : std::string{};
            void* ptr = nullptr;
            const bool matched =
                types::is_opaque(a) && !tag.empty() && handle_probe.match != nullptr &&
                handle_probe.match(handle_probe.ctx, types::as_opaque_idx(a), tag, ptr);
            if (!matched) {
                out.bad_arg = static_cast<int>(i);
                out.bad_kind = 5;
                out.bad_tag = tag.empty() ? std::string{"handle"} : tag;
                return out;
            }
            out.i_vals[i] = reinterpret_cast<std::int64_t>(ptr);
        } else { // Int (atype 1 or unknown)
            if (!types::is_int(a)) {
                out.bad_arg = static_cast<int>(i);
                out.bad_kind = 1;
                return out;
            }
            out.i_vals[i] = types::as_int(a);
            out.d_vals[i] = static_cast<double>(out.i_vals[i]);
        }
        out.ic[static_cast<std::size_t>(out.ni++)] = out.i_vals[i];
    }
    return out;
}

// ffi_call_by_class — Issue #4531: call a foreign fn through a prototype
// matching its DECLARED slot classes. SysV assigns integer args (rdi, rsi,
// rdx, rcx, r8, r9) and SSE args (xmm0..7) independently per class, so an
// int-first prototype is register-compatible with any interleaving of the
// same (ni, nd) shape: the i-th integer slot lands in the i-th integer
// register and the j-th Float slot in xmm(j), whatever the declared order.
// A Float result lives in xmm0 and an integer result in rax regardless of
// the argument classes.
//
// Issue #4553: integer-class slots past the register count are passed on
// the stack in declared order, and that also holds for an int-first
// prototype as long as no Float spills (nd <= 8 keeps every Float in
// xmm0..7). The old hand-written switch stopped at ni + nd <= 6. One
// prototype per (ni, nd) shape is generated here: ni in [0,
// kFfiMaxIntSlots], nd in [0, kFfiMaxSseSlots]; anything else returns R{}.
namespace ffi_detail {
    template <std::size_t> struct FfiIntSlot {
        using type = std::int64_t;
    };
    template <std::size_t> struct FfiSseSlot {
        using type = double;
    };
    template <typename R, std::size_t... I, std::size_t... J>
    inline R ffi_call_shape(void* fn, const std::int64_t* ic, const double* dc,
                            std::index_sequence<I...>, std::index_sequence<J...>) {
        using F = R (*)(typename FfiIntSlot<I>::type..., typename FfiSseSlot<J>::type...);
        (void)ic;
        (void)dc;
        return reinterpret_cast<F>(fn)(ic[I]..., dc[J]...);
    }
    template <typename R, std::size_t K>
    R ffi_call_shape_k(void* fn, const std::int64_t* ic, const double* dc) {
        return ffi_call_shape<R>(fn, ic, dc, std::make_index_sequence<K / (kFfiMaxSseSlots + 1)>{},
                                 std::make_index_sequence<K % (kFfiMaxSseSlots + 1)>{});
    }
    template <typename R> using FfiShapeFn = R (*)(void*, const std::int64_t*, const double*);
    template <typename R, std::size_t... K>
    constexpr std::array<FfiShapeFn<R>, sizeof...(K)> ffi_shape_table(std::index_sequence<K...>) {
        return {&ffi_call_shape_k<R, K>...};
    }
} // namespace ffi_detail

export template <typename R>
inline R ffi_call_by_class(void* fn, const std::int64_t* ic, const double* dc, int ni, int nd) {
    if (ni < 0 || nd < 0 || ni > static_cast<int>(kFfiMaxIntSlots) ||
        nd > static_cast<int>(kFfiMaxSseSlots))
        return R{};
    static constexpr auto kTable = ffi_detail::ffi_shape_table<R>(
        std::make_index_sequence<(kFfiMaxIntSlots + 1) * (kFfiMaxSseSlots + 1)>{});
    return kTable[static_cast<std::size_t>(ni) * (kFfiMaxSseSlots + 1) +
                  static_cast<std::size_t>(nd)](fn, ic, dc);
}

// ── Macro expansion (Issue #146 Phase 6 sub-extract) ────────
//
// `macro_expand_all` is a 500+ line stateful function — too
// large to extract as a single pure function in one PR. We
// start with the small, self-contained helpers used by
// the inner-macro recursion (Issue #121). Future PRs can
// extract more pieces as confidence grows.

// compute_macro_subst_pure — builds the substitution map
// (param name → arg NodeId) for a single macro-call site.
// Given the macro's parameter list and the call's child
// NodeIds (children[1..] of the Call node), produce the
// param→arg mapping for the regular (non-dotted) parameters.
// Dotted-rest-param handling is left to the caller because
// it requires allocating a new pair-list in the FlatAST
// (stateful — not pure).
//
// Returns the substitution map for regular params. For
// dotted macros, the caller is expected to extend this
// map with the rest-param binding (a synthesized pair
// list) after this returns.
export inline std::unordered_map<std::string, aura::ast::NodeId, aura::core::TransparentStringHash,
                                 std::equal_to<>>
compute_macro_subst_pure(const std::vector<std::string>& params,
                         const std::vector<aura::ast::NodeId>& call_args, bool dotted) {
    using aura::ast::NodeId;
    std::unordered_map<std::string, NodeId, aura::core::TransparentStringHash, std::equal_to<>>
        subst;
    std::size_t regular_count = dotted && params.size() > 0 ? params.size() - 1 : params.size();
    for (std::size_t ai = 0; ai < regular_count && ai + 1 < call_args.size(); ++ai) {
        subst[params[ai]] = call_args[ai + 1];
    }
    return subst;
}

} // namespace aura::compiler::pure

// Re-export the pure is_truthy as types::is_truthy for
// backward compat (legacy callers in evaluator partition TUs
// unqualified-ADL through types::). New code should
// import aura.compiler.evaluator_pure and use
// aura::compiler::pure::is_truthy directly.
//
// Note: using-declaration (not using-alias) because the
// target is a function, not a type.
namespace aura::compiler::types {
using ::aura::compiler::pure::is_truthy;
}

namespace aura::compiler::pure {

// coerce_to_int_pure — Issue #146 (first extract).
//
// Pure coercion: EvalValue → std::int64_t, with the string heap
// provided as a parameter (no Evaluator member access).
//
// Returns `Result<int64_t, Diagnostic>`. Errors are produced for
// cases where coercion is well-defined but unparseable:
//   - STRING → int: when the string is not a valid integer
//
// Silently coerces the "well-defined" cases to 0:
//   - VOID, PAIR, CLOSURE, etc. → 0 (no error — matches
//     legacy `coerce_to_int` which also returned 0 for these)
//
// This matches the legacy `coerce_to_int` semantics for callers
// that ignore the Result, but adds an error path for callers
// that want to handle string-parse failures explicitly.
export inline aura::diag::Result<std::int64_t> coerce_to_int_pure(const types::EvalValue& v,
                                                                  std::span<const std::string> heap)
    // Issue #213 follow-up: C++26 contract. The function
    // gracefully handles v being any EvalValue (returns 0 for
    // non-coercible types), so the precondition is just
    // that the function never panics. We assert the heap is
    // well-formed IF the value is a string (the only case
    // that uses the heap) — but this is a soft precondition
    // (the function falls back to 0 on bad heap indices, so
    // there's no hard requirement here).
    pre(true) // function is total; heap is consulted only for string values
{
    if (types::is_int(v))
        return types::as_int(v);
    if (types::is_float(v)) {
        // Issue #1153: out-of-range float→int is UB via static_cast; saturate.
        const double d = types::as_float(v);
        constexpr double kMax = static_cast<double>(std::numeric_limits<std::int64_t>::max());
        constexpr double kMin = static_cast<double>(std::numeric_limits<std::int64_t>::min());
        if (!(d == d) || d >= kMax) // NaN or >= max
            return std::numeric_limits<std::int64_t>::max();
        if (d <= kMin)
            return std::numeric_limits<std::int64_t>::min();
        return static_cast<std::int64_t>(d);
    }
    if (types::is_string(v)) {
        if (heap.empty())
            return 0; // no heap to resolve index — silent fallback
        auto idx = types::as_string_idx(v);
        if (idx >= heap.size())
            return 0; // out-of-bounds index — silent fallback
        try {
            // Issue #1154: reject out-of-range parsed values.
            const long long parsed = std::stoll(heap[idx]);
            if (parsed > static_cast<long long>(std::numeric_limits<std::int64_t>::max()) ||
                parsed < static_cast<long long>(std::numeric_limits<std::int64_t>::min())) {
                return std::unexpected(aura::diag::Diagnostic(
                    aura::diag::ErrorKind::TypeError,
                    std::string("coerce: string integer out of int64 range: '") + heap[idx] + "'"));
            }
            return static_cast<std::int64_t>(parsed);
        } catch (const std::exception&) {
            return std::unexpected(aura::diag::Diagnostic(
                aura::diag::ErrorKind::TypeError,
                std::string("coerce: cannot parse string '") + heap[idx] + "' as integer"));
        }
    }
    if (types::is_bool(v))
        return types::as_bool(v) ? 1 : 0;
    return 0; // void, pair, closure, etc. — silent 0
}

// coerce_value_pure — Issue #146 Phase 2 extract.
//
// Pure multi-target coercion. Mirrors the legacy `coerce_value`
// signature (val in-out, heap in-out for INT→STRING / FLOAT→STRING
// push_back) but returns Result<void> instead of bool — callers
// can compose monadically (.and_then, .or_else) and the error path
// carries a Diagnostic explaining the non-coercible case.
//
// "Pure" here means no Evaluator member access: all dependencies
// (val, from, to, heap) are passed in as parameters. Heap mutation
// for INT→STRING / FLOAT→STRING is allowed (and expected) since
// heap is a parameter, not Evaluator state. The legacy in-out
// `coerce_value` in evaluator_eval_flat.cpp becomes a thin wrapper.
export inline aura::diag::Result<void> coerce_value_pure(types::EvalValue& val,
                                                         aura::core::TypeTag from,
                                                         aura::core::TypeTag to,
                                                         ::aura::compiler::LockedStringHeap& heap) {
    using aura::core::TypeTag;
    if (from == to)
        return {};
    if (from == TypeTag::INT && to == TypeTag::FLOAT) {
        val = types::make_float(static_cast<double>(types::as_int(val)));
        return {};
    }
    if (from == TypeTag::FLOAT && to == TypeTag::INT) {
        // Issue #1155: saturate float→int (no out-of-range static_cast UB).
        const double d = types::as_float(val);
        constexpr double kMax = static_cast<double>(std::numeric_limits<std::int64_t>::max());
        constexpr double kMin = static_cast<double>(std::numeric_limits<std::int64_t>::min());
        std::int64_t i = 0;
        if (!(d == d) || d >= kMax)
            i = std::numeric_limits<std::int64_t>::max();
        else if (d <= kMin)
            i = std::numeric_limits<std::int64_t>::min();
        else
            i = static_cast<std::int64_t>(d);
        // Issue #4462: a saturated int64 is outside the fixnum domain.
        // Keep the original float instead of aborting in make_int.
        if (!types::fits_fixnum(i)) {
            val = types::make_float(d);
            return {};
        }
        val = types::make_int(i);
        return {};
    }
    if (from == TypeTag::INT && to == TypeTag::STRING) {
        auto s = std::to_string(types::as_int(val));
        auto id = static_cast<std::uint64_t>(heap.mint(std::move(s)));
        val = types::make_string(id);
        return {};
    }
    if (from == TypeTag::STRING && to == TypeTag::INT) {
        auto idx = types::as_string_idx(val);
        if (idx < heap.size()) {
            try {
                // Issue #1154: stoll throws out_of_range for overflow; no cast UB.
                const long long parsed = std::stoll(heap[static_cast<std::size_t>(idx)]);
                if (parsed > static_cast<long long>(std::numeric_limits<std::int64_t>::max()) ||
                    parsed < static_cast<long long>(std::numeric_limits<std::int64_t>::min())) {
                    return std::unexpected(
                        aura::diag::Diagnostic(aura::diag::ErrorKind::TypeError,
                                               "coerce_value: string integer out of int64 range"));
                }
                const auto iv = static_cast<std::int64_t>(parsed);
                // Issue #4462: in int64 but outside the fixnum domain.
                val = types::fits_fixnum(iv) ? types::make_int(iv)
                                             : types::make_float(static_cast<double>(iv));
                return {};
            } catch (const std::exception&) {
                return std::unexpected(aura::diag::Diagnostic(
                    aura::diag::ErrorKind::TypeError,
                    "coerce_value: cannot parse string '" +
                        std::string(heap[static_cast<std::size_t>(idx)]) + "' as integer"));
            }
        }
        return std::unexpected(aura::diag::Diagnostic(aura::diag::ErrorKind::TypeError,
                                                      "coerce_value: string index out of bounds"));
    }
    if (from == TypeTag::INT && to == TypeTag::BOOL) {
        val = types::make_bool(types::as_int(val) != 0);
        return {};
    }
    if (from == TypeTag::BOOL && to == TypeTag::INT) {
        val = types::make_int(types::as_bool(val) ? 1 : 0);
        return {};
    }
    if (from == TypeTag::FLOAT && to == TypeTag::STRING) {
        auto s = std::to_string(types::as_float(val));
        auto id = static_cast<std::uint64_t>(heap.mint(std::move(s)));
        val = types::make_string(id);
        return {};
    }
    if (from == TypeTag::STRING && to == TypeTag::FLOAT) {
        auto idx = types::as_string_idx(val);
        if (idx < heap.size()) {
            try {
                val = types::make_float(std::stod(heap[static_cast<std::size_t>(idx)]));
                return {};
            } catch (const std::exception&) {
                return std::unexpected(aura::diag::Diagnostic(
                    aura::diag::ErrorKind::TypeError,
                    "coerce_value: cannot parse string '" +
                        std::string(heap[static_cast<std::size_t>(idx)]) + "' as float"));
            }
        }
        return std::unexpected(aura::diag::Diagnostic(aura::diag::ErrorKind::TypeError,
                                                      "coerce_value: string index out of bounds"));
    }
    return std::unexpected(aura::diag::Diagnostic(aura::diag::ErrorKind::TypeError,
                                                  "coerce_value: unsupported coercion"));
}

// Issue #4463: arithmetic accepts only ints and floats. Bool, void, pair,
// vector, closure, error, and every string (including "2.5" and "12abc")
// are non-numbers. Annotation coercion stays in coerce_value_pure.
export inline bool is_arith_number(const types::EvalValue& v) noexcept {
    return types::is_int(v) || types::is_float(v);
}

export inline const types::EvalValue*
first_non_number(std::span<const types::EvalValue> args) noexcept {
    for (const auto& v : args) {
        if (!is_arith_number(v))
            return &v;
    }
    return nullptr;
}

export inline const char* numeric_operand_type_name(const types::EvalValue& v) noexcept {
    if (types::is_bool(v))
        return "Bool";
    if (types::is_void(v))
        return "Void";
    if (types::is_pair(v))
        return "Pair";
    if (types::is_vector(v))
        return "Vector";
    if (types::is_closure(v))
        return "Closure";
    if (types::is_error(v))
        return "Error";
    if (types::is_string(v))
        return "String";
    return "Value";
}

// Legacy stderr text, without the "error: " prefix. Strings keep the
// exact "got String '…'" form (em dash). Callers print `error: ` + this.
export inline std::string numeric_type_mismatch_detail(const types::EvalValue& v,
                                                       std::string_view string_text) {
    if (types::is_string(v)) {
        return std::string("type mismatch — expected Int, got String '") +
               std::string(string_text) + "'";
    }
    return std::string("type mismatch — expected Int, got ") + numeric_operand_type_name(v);
}

inline std::string_view arith_string_text(const types::EvalValue& v,
                                          std::span<const std::string> heap) noexcept {
    if (!types::is_string(v) || heap.empty())
        return {};
    const auto idx = types::as_string_idx(v);
    if (idx >= heap.size())
        return {};
    return heap[idx];
}

// Issue #4463: a non-number is not a sum. make_error(0) is the sentinel
// the primitive wrapper replaces with a cause string. Numeric results
// are never error values.
inline types::EvalValue arith_type_sentinel(const types::EvalValue& bad,
                                            std::span<const std::string> heap, std::ostream* diag) {
    if (diag) {
        *diag << "error: " << numeric_type_mismatch_detail(bad, arith_string_text(bad, heap))
              << '\n';
    }
    return types::make_error(0);
}

// Issue #4482: fixnum overflow is an error, not a silent inexact float.
// Fixnums cover [-2^62, 2^62-1]; there is no bignum tier yet.
inline types::EvalValue fixnum_overflow_error(const char* op, std::ostream* diag) {
    if (diag)
        *diag << "error: integer overflow in " << op << " (fixnum range is [-2^62, 2^62-1])\n";
    return types::make_error(0);
}

// arithmetic_sum_pure — Issue #146 (7th extract).
//
// Pure variadic sum: (+) → 0, (+ x) → x, (+ x y ...) → sum.
// Promotes to float if any arg is float; otherwise stays int.
// Returns the result as a fresh EvalValue (make_int or make_float).
//
// `diag` is an optional diagnostics sink. When non-null, a non-number
// operand writes the legacy "type mismatch" line (regression tests
// tc-strict-runtime-typed-arg-mismatch and tc-type-var-name-from-param
// substring-match stderr). The result is an error sentinel, not 0.
export inline types::EvalValue arithmetic_sum_pure(std::span<const types::EvalValue> args,
                                                   std::span<const std::string> string_heap,
                                                   std::ostream* diag = nullptr) {
    if (args.empty())
        return types::make_int(0);
    if (const auto* bad = first_non_number(args))
        return arith_type_sentinel(*bad, string_heap, diag);
    bool any_f = false;
    for (const auto& v : args) {
        if (types::is_float(v)) {
            any_f = true;
            break;
        }
    }
    if (any_f) {
        double r = 0.0;
        for (const auto& v : args)
            r += types::is_float(v) ? types::as_float(v) : static_cast<double>(types::as_int(v));
        return types::make_float(r);
    }
    // Issue #1150 / #4482: checked int64 sum — overflow is an error (no UB).
    std::int64_t r = 0;
    for (const auto& v : args) {
        const std::int64_t op = types::as_int(v);
        std::int64_t next = 0;
        // Issue #4462: [2^62, 2^63) passes the int64 check and is not a fixnum.
        if (__builtin_add_overflow(r, op, &next) || !types::fits_fixnum(next))
            return fixnum_overflow_error("+", diag);
        r = next;
    }
    return types::make_int(r);
}

// arithmetic_sub_pure — Issue #212 Phase 3.
//
// Pure variadic subtraction: (-) → 0, (- x) → -x, (- x y z ...) → x - y - z.
// Promotes to float if any arg is float. Returns the result as a
// fresh EvalValue (make_int or make_float). Same diag sink
// convention as arithmetic_sum_pure.
export inline types::EvalValue arithmetic_sub_pure(std::span<const types::EvalValue> args,
                                                   std::span<const std::string> string_heap,
                                                   std::ostream* diag = nullptr) {
    if (args.empty())
        return types::make_int(0);
    if (const auto* bad = first_non_number(args))
        return arith_type_sentinel(*bad, string_heap, diag);
    bool any_f = false;
    for (const auto& v : args) {
        if (types::is_float(v)) {
            any_f = true;
            break;
        }
    }
    if (any_f) {
        if (args.size() == 1)
            return types::make_float(-(types::is_float(args[0])
                                           ? types::as_float(args[0])
                                           : static_cast<double>(types::as_int(args[0]))));
        double r = types::is_float(args[0]) ? types::as_float(args[0])
                                            : static_cast<double>(types::as_int(args[0]));
        for (std::size_t i = 1; i < args.size(); ++i)
            r -= types::is_float(args[i]) ? types::as_float(args[i])
                                          : static_cast<double>(types::as_int(args[i]));
        return types::make_float(r);
    }
    // Issue #1150: checked int64 sub — overflow/underflow promotes to float.
    if (args.size() == 1) {
        const std::int64_t a0 = types::as_int(args[0]);
        std::int64_t neg = 0;
        if (__builtin_sub_overflow(static_cast<std::int64_t>(0), a0, &neg) ||
            !types::fits_fixnum(neg))
            return fixnum_overflow_error("-", diag);
        return types::make_int(neg);
    }
    std::int64_t r = types::as_int(args[0]);
    if (!types::fits_fixnum(r))
        return fixnum_overflow_error("-", diag);
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::int64_t op = types::as_int(args[i]);
        std::int64_t next = 0;
        if (__builtin_sub_overflow(r, op, &next) || !types::fits_fixnum(next))
            return fixnum_overflow_error("-", diag);
        r = next;
    }
    return types::make_int(r);
}

// arithmetic_mul_pure — Issue #212 Phase 3.
//
// Pure variadic multiplication: (*) → 1, (* x) → x, (* x y z ...) → x * y * z.
// Promotes to float if any arg is float.
export inline types::EvalValue arithmetic_mul_pure(std::span<const types::EvalValue> args,
                                                   std::span<const std::string> string_heap,
                                                   std::ostream* diag = nullptr) {
    if (args.empty())
        return types::make_int(1);
    if (const auto* bad = first_non_number(args))
        return arith_type_sentinel(*bad, string_heap, diag);
    bool any_f = false;
    for (const auto& v : args) {
        if (types::is_float(v)) {
            any_f = true;
            break;
        }
    }
    if (any_f) {
        double r = 1.0;
        for (const auto& v : args)
            r *= types::is_float(v) ? types::as_float(v) : static_cast<double>(types::as_int(v));
        return types::make_float(r);
    }
    // Issue #1150/#1152: checked int64 mul — overflow promotes to float.
    std::int64_t r = 1;
    for (const auto& v : args) {
        const std::int64_t op = types::as_int(v);
        std::int64_t next = 0;
        if (__builtin_mul_overflow(r, op, &next) || !types::fits_fixnum(next))
            return fixnum_overflow_error("*", diag);
        r = next;
    }
    return types::make_int(r);
}

// arithmetic_div_pure — Issue #212 Phase 3.
//
// Pure variadic division. Returns Result<EvalValue> because
// an all-integer zero divisor is a runtime error the caller
// surfaces (Issue #4461: an error value, not a silent 0).
// A float zero divisor is IEEE 754: +inf, -inf, or nan.
//
// `nonzero_count` out-param: when non-null, set to the count
// of args that were non-zero. Useful for callers that want to
// short-circuit on all-zeros.
export inline aura::diag::Result<types::EvalValue>
arithmetic_div_pure(std::span<const types::EvalValue> args,
                    std::span<const std::string> string_heap, std::ostream* diag = nullptr)
    // Issue #213 follow-up: C++26 contract. Division by
    // zero is detected at runtime via std::unexpected; the
    // function gracefully handles empty args (returns
    // TypeError), so the only precondition is that the
    // span's data() is non-null (i.e. not a default-constructed
    // span with a null pointer).
    pre(args.data() != nullptr || args.empty()) {
    // Issue #2940: zero-check must NOT truncate floats. |x| < 1 is not 0.
    // Issue #4463: a non-number is not a zero divisor.
    auto is_zero = [](const types::EvalValue& v) -> bool {
        if (types::is_float(v)) {
            const double d = types::as_float(v);
            return d == 0.0; // +0.0 and -0.0
        }
        if (types::is_int(v))
            return types::as_int(v) == 0;
        return false;
    };
    if (args.empty()) {
        return std::unexpected(aura::diag::Diagnostic(aura::diag::ErrorKind::TypeError,
                                                      "division: at least one argument required"));
    }
    if (const auto* bad = first_non_number(args)) {
        const auto detail =
            numeric_type_mismatch_detail(*bad, arith_string_text(*bad, string_heap));
        if (diag)
            *diag << "error: " << detail << '\n';
        return std::unexpected(aura::diag::Diagnostic(aura::diag::ErrorKind::TypeError, detail));
    }
    // Issue #1151/#1156: single-arg reciprocal; Issue #2940: float reciprocal.
    if (args.size() == 1) {
        if (types::is_float(args[0])) {
            // Issue #4461: IEEE reciprocal. (/ 0.0) is +inf, (/ -0.0) is -inf.
            const double d = types::as_float(args[0]);
            return types::make_float(1.0 / d);
        }
        const std::int64_t d = types::as_int(args[0]);
        if (d == 0) {
            return std::unexpected(
                aura::diag::Diagnostic(aura::diag::ErrorKind::DivisionByZero, "division by zero"));
        }
        if (d == -1) {
            // 1 / -1 is fine; only multi-arg INT64_MIN/-1 is overflow.
        }
        return types::make_int(1 / d); // legacy: 1/x for int
    }

    // Multi-arg: a / b / c / ...
    // Issue #4461: float zero is IEEE (inf/nan). All-integer zero is
    // DivisionByZero. #2940: the zero test stays type-aware so a
    // float with |x| < 1 is not a false zero.
    bool any_f = false;
    for (const auto& v : args) {
        if (types::is_float(v)) {
            any_f = true;
            break;
        }
    }
    if (!any_f) {
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (is_zero(args[i])) {
                return std::unexpected(aura::diag::Diagnostic(aura::diag::ErrorKind::DivisionByZero,
                                                              "division by zero"));
            }
        }
    }
    if (any_f) {
        double r = types::is_float(args[0]) ? types::as_float(args[0])
                                            : static_cast<double>(types::as_int(args[0]));
        for (std::size_t i = 1; i < args.size(); ++i)
            r /= types::is_float(args[i]) ? types::as_float(args[i])
                                          : static_cast<double>(types::as_int(args[i]));
        return types::make_float(r);
    }
    std::int64_t r = types::as_int(args[0]);
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::int64_t d = types::as_int(args[i]);
        if (r == std::numeric_limits<std::int64_t>::min() && d == -1) {
            return std::unexpected(
                aura::diag::Diagnostic(aura::diag::ErrorKind::TypeError, "int64 overflow in /"));
        }
        r /= d;
    }
    return types::make_int(r);
}

} // namespace aura::compiler::pure
