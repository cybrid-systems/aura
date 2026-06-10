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
    if (v.val == 3) return false;           // #f
    if (types::is_int(v) && types::as_int(v) == 0) return false;  // integer 0
    return true;
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
export inline aura::diag::Result<std::int64_t> coerce_to_int_pure(
    const types::EvalValue& v, std::span<const std::string> heap) {
    if (types::is_int(v))
        return types::as_int(v);
    if (types::is_float(v))
        return static_cast<std::int64_t>(types::as_float(v));
    if (types::is_string(v)) {
        if (heap.empty())
            return 0;  // no heap to resolve index — silent fallback
        auto idx = types::as_string_idx(v);
        if (idx >= heap.size())
            return 0;  // out-of-bounds index — silent fallback
        try {
            return static_cast<std::int64_t>(std::stoll(heap[idx]));
        } catch (const std::exception&) {
            return std::unexpected(aura::diag::Diagnostic(
                aura::diag::ErrorKind::TypeError,
                std::string("coerce: cannot parse string '") + heap[idx]
                    + "' as integer"));
        }
    }
    if (types::is_bool(v))
        return types::as_bool(v) ? 1 : 0;
    return 0;  // void, pair, closure, etc. — silent 0
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
export inline aura::diag::Result<void> coerce_value_pure(
    types::EvalValue& val,
    aura::core::TypeTag from,
    aura::core::TypeTag to,
    std::pmr::vector<std::string>& heap)
{
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
                    "coerce_value: cannot parse string '" + std::string(heap[static_cast<std::size_t>(idx)])
                        + "' as integer"));
            }
        }
        return std::unexpected(aura::diag::Diagnostic(
            aura::diag::ErrorKind::TypeError,
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
                val = types::make_float(
                    std::stod(heap[static_cast<std::size_t>(idx)]));
                return {};
            } catch (const std::exception&) {
                return std::unexpected(aura::diag::Diagnostic(
                    aura::diag::ErrorKind::TypeError,
                    "coerce_value: cannot parse string '" + std::string(heap[static_cast<std::size_t>(idx)])
                        + "' as float"));
            }
        }
        return std::unexpected(aura::diag::Diagnostic(
            aura::diag::ErrorKind::TypeError,
            "coerce_value: string index out of bounds"));
    }
    return std::unexpected(aura::diag::Diagnostic(
        aura::diag::ErrorKind::TypeError,
        "coerce_value: unsupported coercion"));
}

} // namespace aura::compiler::pure
