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

} // namespace aura::compiler::pure
