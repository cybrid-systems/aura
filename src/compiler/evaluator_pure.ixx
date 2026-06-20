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
export inline bool is_truthy(const types::EvalValue& v) noexcept {
    if (v.val == 3)
        return false; // #f
    if (types::is_int(v) && types::as_int(v) == 0)
        return false; // integer 0
    return true;
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
//   1 = Int, 2 = Float, 3 = String, 4 = Opaque
struct FFIMarshalled {
    std::array<std::int64_t, 6> i_vals{}; // raw i64 registers
    std::array<double, 6> d_vals{};       // raw f64 registers
    std::array<const char*, 6> s_vals{};  // char* registers (point into str_bufs)
    std::vector<std::string> str_bufs;    // owns string contents
    bool any_float = false;
};

// ffi_marshal_args_pure — marshals up to 6 EvalValue args
// into the raw C ABI for a foreign-function call. The
// `str_bufs` field on the result owns the underlying
// string memory for `s_vals` pointers; the caller must
// keep the result alive for the duration of the FFI call.
//
// arg_type codes: 1 = Int (default if missing), 2 = Float,
// 3 = String (looks up idx in string_heap, copies into
// str_bufs for lifetime), 4 = Opaque (looks up idx in
// opaque_heap, treats the void* as i64).
//
// All inputs are read-only spans. No `this` access.
export inline FFIMarshalled ffi_marshal_args_pure(std::span<const types::EvalValue> args,
                                                  std::span<const int> arg_types,
                                                  std::span<const std::string> string_heap,
                                                  std::span<void* const> opaque_heap) {
    FFIMarshalled out{};
    constexpr std::size_t kMaxArgs = 6;
    for (std::size_t i = 0; i < args.size() && i < kMaxArgs; ++i) {
        int atype = (i < arg_types.size()) ? arg_types[i] : 1; // default Int
        const auto& a = args[i];
        if (atype == 2) { // Float
            if (types::is_float(a)) {
                out.d_vals[i] = types::as_float(a);
            } else if (types::is_int(a)) {
                out.d_vals[i] = static_cast<double>(types::as_int(a));
            }
            out.i_vals[i] = static_cast<std::int64_t>(out.d_vals[i]);
            out.any_float = true;
        } else if (atype == 3) { // String → char*
            if (types::is_string(a)) {
                auto idx = types::as_string_idx(a);
                if (idx < string_heap.size()) {
                    out.str_bufs.push_back(string_heap[idx]);
                    out.s_vals[i] = out.str_bufs.back().c_str();
                    out.i_vals[i] = reinterpret_cast<std::int64_t>(out.s_vals[i]);
                    out.d_vals[i] = 0.0;
                }
            }
        } else if (atype == 4) { // Opaque (void*)
            if (types::is_opaque(a)) {
                auto oi = types::as_opaque_idx(a);
                out.i_vals[i] =
                    oi < opaque_heap.size() ? reinterpret_cast<std::int64_t>(opaque_heap[oi]) : 0;
            } else if (types::is_int(a)) {
                out.i_vals[i] = types::as_int(a);
            } else {
                out.i_vals[i] = 0;
            }
            out.d_vals[i] = 0.0;
        } else { // Int (default for atype 1 or unknown)
            if (types::is_int(a)) {
                out.i_vals[i] = types::as_int(a);
            } else if (types::is_float(a)) {
                out.i_vals[i] = static_cast<std::int64_t>(types::as_float(a));
                out.any_float = true;
            }
            out.d_vals[i] = static_cast<double>(out.i_vals[i]);
        }
    }
    return out;
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
export inline std::unordered_map<std::string, aura::ast::NodeId>
compute_macro_subst_pure(const std::vector<std::string>& params,
                         const std::vector<aura::ast::NodeId>& call_args, bool dotted) {
    using aura::ast::NodeId;
    std::unordered_map<std::string, NodeId> subst;
    std::size_t regular_count = dotted && params.size() > 0 ? params.size() - 1 : params.size();
    for (std::size_t ai = 0; ai < regular_count && ai + 1 < call_args.size(); ++ai) {
        subst[params[ai]] = call_args[ai + 1];
    }
    return subst;
}

} // namespace aura::compiler::pure

// Re-export the pure is_truthy as types::is_truthy for
// backward compat (legacy callers in evaluator_impl.cpp
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
    if (types::is_float(v))
        return static_cast<std::int64_t>(types::as_float(v));
    if (types::is_string(v)) {
        if (heap.empty())
            return 0; // no heap to resolve index — silent fallback
        auto idx = types::as_string_idx(v);
        if (idx >= heap.size())
            return 0; // out-of-bounds index — silent fallback
        try {
            return static_cast<std::int64_t>(std::stoll(heap[idx]));
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
// `coerce_value` in evaluator_impl.cpp becomes a thin wrapper.
export inline aura::diag::Result<void> coerce_value_pure(types::EvalValue& val,
                                                         aura::core::TypeTag from,
                                                         aura::core::TypeTag to,
                                                         std::pmr::vector<std::string>& heap) {
    using aura::core::TypeTag;
    if (from == to)
        return {};
    if (from == TypeTag::INT && to == TypeTag::FLOAT) {
        val = types::make_float(static_cast<double>(types::as_int(val)));
        return {};
    }
    if (from == TypeTag::FLOAT && to == TypeTag::INT) {
        val = types::make_int(static_cast<std::int64_t>(types::as_float(val)));
        return {};
    }
    if (from == TypeTag::INT && to == TypeTag::STRING) {
        auto s = std::to_string(types::as_int(val));
        auto id = static_cast<std::uint64_t>(heap.size());
        heap.push_back(std::move(s));
        val = types::make_string(id);
        return {};
    }
    if (from == TypeTag::STRING && to == TypeTag::INT) {
        auto idx = types::as_string_idx(val);
        if (idx < heap.size()) {
            try {
                val = types::make_int(
                    static_cast<std::int64_t>(std::stoll(heap[static_cast<std::size_t>(idx)])));
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
        auto id = static_cast<std::uint64_t>(heap.size());
        heap.push_back(std::move(s));
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

// arithmetic_sum_pure — Issue #146 (7th extract).
//
// Pure variadic sum: (+) → 0, (+ x) → x, (+ x y ...) → sum.
// Promotes to float if any arg is float; otherwise stays int.
// Returns the result as a fresh EvalValue (make_int or make_float).
//
// `diag` is an optional diagnostics sink. When non-null, the
// function writes the legacy \"type mismatch\" line to *diag on
// any arg whose coercion to int fails (matches pre-extract
// stateful behavior — regression tests
// tc-strict-runtime-typed-arg-mismatch and
// tc-type-var-name-from-param depend on this). When null
// (default), no diagnostics are emitted. The function remains
// pure modulo the sink: every dependency (args, heap, diag
// stream) is passed in, no `this` or Evaluator member access.
export inline types::EvalValue arithmetic_sum_pure(std::span<const types::EvalValue> args,
                                                   std::span<const std::string> string_heap,
                                                   std::ostream* diag = nullptr) {
    auto coerce_one = [&](const types::EvalValue& v) -> std::int64_t {
        auto r = coerce_to_int_pure(v, string_heap);
        if (r)
            return *r;
        // Coercion failed. If the failing value is a String and
        // a diag sink is provided, mirror the pre-Phase-1 stderr
        // line so callers that previously relied on the
        // stateful coerce_to_int wrapper's stderr emission
        // (e.g. the `+` primitive in evaluator_impl.cpp) keep
        // working.
        if (diag && types::is_string(v) && !string_heap.empty()) {
            auto idx = types::as_string_idx(v);
            if (idx < string_heap.size()) {
                *diag << "error: type mismatch — expected Int, got String '" << string_heap[idx]
                      << "'\n";
            }
        }
        return 0;
    };
    if (args.empty())
        return types::make_int(0);
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
            r += types::is_float(v) ? types::as_float(v) : static_cast<double>(coerce_one(v));
        return types::make_float(r);
    }
    std::int64_t r = 0;
    for (const auto& v : args)
        r += coerce_one(v);
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
    auto coerce_one = [&](const types::EvalValue& v) -> std::int64_t {
        auto r = coerce_to_int_pure(v, string_heap);
        if (r)
            return *r;
        if (diag && types::is_string(v) && !string_heap.empty()) {
            auto idx = types::as_string_idx(v);
            if (idx < string_heap.size()) {
                *diag << "error: type mismatch — expected Int, got String '" << string_heap[idx]
                      << "'\n";
            }
        }
        return 0;
    };
    if (args.empty())
        return types::make_int(0);
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
                                           : static_cast<double>(coerce_one(args[0]))));
        double r = types::is_float(args[0]) ? types::as_float(args[0])
                                            : static_cast<double>(coerce_one(args[0]));
        for (std::size_t i = 1; i < args.size(); ++i)
            r -= types::is_float(args[i]) ? types::as_float(args[i])
                                          : static_cast<double>(coerce_one(args[i]));
        return types::make_float(r);
    }
    if (args.size() == 1)
        return types::make_int(-coerce_one(args[0]));
    std::int64_t r = coerce_one(args[0]);
    for (std::size_t i = 1; i < args.size(); ++i)
        r -= coerce_one(args[i]);
    return types::make_int(r);
}

// arithmetic_mul_pure — Issue #212 Phase 3.
//
// Pure variadic multiplication: (*) → 1, (* x) → x, (* x y z ...) → x * y * z.
// Promotes to float if any arg is float.
export inline types::EvalValue arithmetic_mul_pure(std::span<const types::EvalValue> args,
                                                   std::span<const std::string> string_heap,
                                                   std::ostream* diag = nullptr) {
    auto coerce_one = [&](const types::EvalValue& v) -> std::int64_t {
        auto r = coerce_to_int_pure(v, string_heap);
        if (r)
            return *r;
        if (diag && types::is_string(v) && !string_heap.empty()) {
            auto idx = types::as_string_idx(v);
            if (idx < string_heap.size()) {
                *diag << "error: type mismatch — expected Int, got String '" << string_heap[idx]
                      << "'\n";
            }
        }
        return 0;
    };
    if (args.empty())
        return types::make_int(1);
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
            r *= types::is_float(v) ? types::as_float(v) : static_cast<double>(coerce_one(v));
        return types::make_float(r);
    }
    std::int64_t r = 1;
    for (const auto& v : args)
        r *= coerce_one(v);
    return types::make_int(r);
}

// arithmetic_div_pure — Issue #212 Phase 3.
//
// Pure variadic division. Returns Result<EvalValue> because
// division by zero is a runtime error that the caller can
// decide how to handle (legacy: silent #inf / #nan for floats,
// trap for ints; the result type lets new code report the
// error explicitly via .error()).
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
    auto coerce_one = [&](const types::EvalValue& v) -> std::int64_t {
        auto r = coerce_to_int_pure(v, string_heap);
        if (r)
            return *r;
        if (diag && types::is_string(v) && !string_heap.empty()) {
            auto idx = types::as_string_idx(v);
            if (idx < string_heap.size()) {
                *diag << "error: type mismatch — expected Int, got String '" << string_heap[idx]
                      << "'\n";
            }
        }
        return 0;
    };
    if (args.empty()) {
        return std::unexpected(aura::diag::Diagnostic(aura::diag::ErrorKind::TypeError,
                                                      "division: at least one argument required"));
    }
    if (args.size() == 1)
        return types::make_int(1 / coerce_one(args[0])); // legacy: 1/x

    // Multi-arg: a / b / c / ...
    // Check all divisors (args[1..]) for zero first; bail out
    // before mutating any state.
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (coerce_one(args[i]) == 0) {
            return std::unexpected(
                aura::diag::Diagnostic(aura::diag::ErrorKind::DivisionByZero, "division by zero"));
        }
    }

    bool any_f = false;
    for (const auto& v : args) {
        if (types::is_float(v)) {
            any_f = true;
            break;
        }
    }
    if (any_f) {
        double r = types::is_float(args[0]) ? types::as_float(args[0])
                                            : static_cast<double>(coerce_one(args[0]));
        for (std::size_t i = 1; i < args.size(); ++i)
            r /= types::is_float(args[i]) ? types::as_float(args[i])
                                          : static_cast<double>(coerce_one(args[i]));
        return types::make_float(r);
    }
    std::int64_t r = coerce_one(args[0]);
    for (std::size_t i = 1; i < args.size(); ++i)
        r /= coerce_one(args[i]);
    return types::make_int(r);
}

} // namespace aura::compiler::pure
