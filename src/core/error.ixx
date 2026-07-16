// src/core/error.ixx — Issue #474: Aura unified error type
// (Phase 0 foundation, scope-limited close).
//
// POLICY (Issues #807/#809): Hot paths use AuraResult / EvalResult;
// exceptions only for OOM / init / hard invariants. Authoritative
// doc: docs/design/error-handling-policy.md
//       docs/design/core/exception_policy.md
//
// The Aura codebase mixes three error-handling patterns
// (std::expected<Result, Diagnostic>, throw / catch /
// std::logic_error, contract_assert) with no documented
// boundary rules. This module ships the FOUNDATION for
// unification:
//
//   1. AuraErrorKind — flat enum of all error categories
//      (subsumes compiler::diag::ErrorKind for the
//      category tag, plus a new MutationError category
//      that absorbs aura::ast::MutationError).
//   2. AuraError — flat struct with kind + message +
//      source_location + (optional) generation tag.
//      The struct is trivially copyable when message is
//      SSO (small string) — std::expected requires it.
//   3. AuraResult<T> = std::expected<T, AuraError> — the
//      unified result alias.
//   4. VoidResult = AuraResult<void>.
//   5. make_unexpected(kind, msg, loc) — convenience
//      builder.
//
// Phase 1 (next issue) migrates hot-path APIs to
// AuraResult. Phase 2 spreads to upper modules.
// Phase 3 handles boundaries (REPL/fiber/COW).
//
// Why std::expected, not exceptions:
//   - AI Agent control loop needs predictable state.
//   - Exceptions break fiber / COW reference counts /
//     generation invalidation chains on unwind.
//   - std::expected + monadic ops makes the failure
//     path explicit in the type system.
//
// Why not just use Diagnostic:
//   - Diagnostic is compiler-specific (compiler/diag.ixx).
//   - We need a core-level type usable from aura::ast,
//     aura::core, and downstream crates.
//   - AuraError is intentionally minimal so it can live
//     in aura.core without circular deps.

module;

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

export module aura.core.error;

import std;

namespace aura::core {

// ── ErrorKind — flat enumeration of all Aura error categories ───────
//
// Subsumes compiler::diag::ErrorKind (compiler-side errors)
// and aura::ast::MutationError (mutation/rollback errors).
// New categories can be appended without breaking ABI
// because callers should switch on the enum value, not
// the ordinal.
//
// Why flat (not std::variant): keeps AuraError trivially
// copyable, simpler switch in REPL/serve layer, easier
// logging/serialization. If a specific error needs rich
// payload in the future, add it as an extra field on
// AuraError (not as a variant alternative).
export enum class AuraErrorKind : std::uint8_t {
    // ── Parser (compiler/diag.ixx) ──
    ParseError,
    UnexpectedToken,
    UnterminatedSExpr,

    // ── Semantic (compiler/diag.ixx) ──
    UnboundVariable,
    DivisionByZero,
    InvalidClosure,
    ArityMismatch,
    TypeError,
    CoercionError,
    OccurrenceTypingError,
    OwnershipError,       // use-after-move / double-borrow
    LinearOwnershipError, // sub-category of Ownership
    PatternMatchExhaustiveness,

    // ── Mutation (aura::ast::MutationError) ──
    MutationNotCommitted,
    MutationNoRollbackData,
    MutationInvalidTarget,
    MutationInvalidParent,
    MutationInvalidField,
    MutationUnknownStructuralOp,
    MutationOutOfRange,

    // ── Arena / allocation ──
    ArenaOutOfMemory,
    ArenaDefragFailed,
    ArenaInvalidAllocator,

    // ── Eval / runtime ──
    EvalError,
    EvalTypeMismatch,
    EvalDivisionByZero,
    EvalStackOverflow,

    // ── Concurrency / fiber ──
    ConcurrencyFiberCanceled,
    ConcurrencyLockFailed,
    ConcurrencyGenerationInvalidated,

    // ── Resource quota (cross-cutting: arena alloc / fiber spawn / mutation
    //    budget / closure capture depth). Issued by #1481 enforcement paths
    //    (arena allocate + MutationBoundaryGuard ctor) when a host-set
    //    ResourceQuota limit is exceeded. The accompanying message carries
    //    the offending dimension (memory / fibers / time / mutations) and the
    //    current vs. limit value so callers can size their next allocation.
    ResourceQuotaExceeded,

    // ── Internal (assertion-class; should never happen in correct code) ──
    InternalInvariantViolation,
    InternalNotImplemented,
    InternalContractFailure,

    // Sentinel — for capacity checks. Always last.
    Sentinel_COUNT_,
};

// ── AuraError — flat struct, trivially copyable ──────────────
//
// 4 fields:
//   - kind: the AuraErrorKind tag (1 byte).
//   - message: small-string-optimized std::string (typically
//     16-24 bytes inline).
//   - location: C++20 std::source_location (8 bytes; captures
//     file/line/column at the call site).
//   - generation: optional cache generation at failure time.
//     Lets users correlate the error with the AST's invalidation
//     state. 0 means "unknown / not relevant".
//
// Total: ~48-64 bytes when message is SSO. std::expected<T,
// AuraError> needs AuraError to be at least move-constructible;
// it is. Trivially-copyable is a stretch goal but not
// required by std::expected.
export struct AuraError {
    AuraErrorKind kind = AuraErrorKind::InternalInvariantViolation;
    std::string message;
    std::source_location location = std::source_location::current();
    std::uint64_t generation = 0;

    AuraError() = default;

    AuraError(AuraErrorKind k, std::string msg,
              std::source_location loc = std::source_location::current(), std::uint64_t gen = 0)
        : kind(k)
        , message(std::move(msg))
        , location(loc)
        , generation(gen) {}

    // Convenience: human-readable name of the kind.
    // Used by REPL print + (query:aura-error-name Kind).
    static std::string_view kind_name(AuraErrorKind k) noexcept;

    // Convenience: human-readable name of THIS error's kind.
    std::string_view kind_name() const noexcept { return kind_name(kind); }
};

// ── AuraResult<T> = std::expected<T, AuraError> ──────────────
//
// The unified result type. All Aura public API boundaries
// should return AuraResult<T> (or VoidResult) instead of
// bool / void / throw.
//
// Why not Result<T> = std::expected<T, Diagnostic> (the
// existing pattern in compiler/diag.ixx): Diagnostic is
// compiler-side (depends on SourceLocation, BlameInfo,
// etc.). AuraError is core-side and can be used by
// aura::ast, aura::core, downstream crates. The two
// can interconvert (see from_diagnostic below).
export template <typename T> using AuraResult = std::expected<T, AuraError>;

// ── VoidResult — for operations that don't produce a value ───
export using VoidResult = AuraResult<void>;

// ── make_unexpected — convenience builder ────────────────────
//
// Returns std::unexpected<AuraError>, so callers can write:
//   return make_unexpected(AuraErrorKind::TypeError, "msg");
//
// The std::unexpected<AuraError> wrapper is what AuraResult
// expects as the error alternative. Using this helper keeps
// the call site readable.
export std::unexpected<AuraError>
make_unexpected(AuraErrorKind k, std::string msg,
                std::source_location loc = std::source_location::current(), std::uint64_t gen = 0);

// ── Issue #807: interop helpers (core-side, no Diagnostic dependency) ──
// Full Diagnostic ↔ AuraError conversion lives in compiler
// (aura_error_bridge) because Diagnostic is aura.diag. Core only
// ships builders that both layers can share.
//
// Policy (see docs/design/core/exception_policy.md):
//   - Hot paths return AuraResult / VoidResult (or EvalResult during
//     migration). Exceptions only for OOM / init / true invariants.
export [[nodiscard]] inline AuraError
make_aura_error(AuraErrorKind k, std::string msg,
                std::source_location loc = std::source_location::current(), std::uint64_t gen = 0) {
    return AuraError{k, std::move(msg), loc, gen};
}

// Build unexpected from a free-form kind name (e.g. diag kind_name()).
// Unknown names map to EvalError so callers never throw on bad tags.
export [[nodiscard]] inline std::unexpected<AuraError>
make_unexpected_from_kind_name(std::string_view kind_name, std::string msg,
                               std::source_location loc = std::source_location::current(),
                               std::uint64_t gen = 0);

// Map a small set of well-known diagnostic kind names → AuraErrorKind.
export [[nodiscard]] inline AuraErrorKind map_kind_name(std::string_view name) noexcept;

} // namespace aura::core

// ═══════════════════════════════════════════════════════════════
// Implementation — kept after the export block so the
// interface is fully visible to consumers.
// ═══════════════════════════════════════════════════════════════

namespace aura::core {

inline std::string_view AuraError::kind_name(AuraErrorKind k) noexcept {
    switch (k) {
        case AuraErrorKind::ParseError:
            return "ParseError";
        case AuraErrorKind::UnexpectedToken:
            return "UnexpectedToken";
        case AuraErrorKind::UnterminatedSExpr:
            return "UnterminatedSExpr";
        case AuraErrorKind::UnboundVariable:
            return "UnboundVariable";
        case AuraErrorKind::DivisionByZero:
            return "DivisionByZero";
        case AuraErrorKind::InvalidClosure:
            return "InvalidClosure";
        case AuraErrorKind::ArityMismatch:
            return "ArityMismatch";
        case AuraErrorKind::TypeError:
            return "TypeError";
        case AuraErrorKind::CoercionError:
            return "CoercionError";
        case AuraErrorKind::OccurrenceTypingError:
            return "OccurrenceTypingError";
        case AuraErrorKind::OwnershipError:
            return "OwnershipError";
        case AuraErrorKind::LinearOwnershipError:
            return "LinearOwnershipError";
        case AuraErrorKind::PatternMatchExhaustiveness:
            return "PatternMatchExhaustiveness";
        case AuraErrorKind::MutationNotCommitted:
            return "MutationNotCommitted";
        case AuraErrorKind::MutationNoRollbackData:
            return "MutationNoRollbackData";
        case AuraErrorKind::MutationInvalidTarget:
            return "MutationInvalidTarget";
        case AuraErrorKind::MutationInvalidParent:
            return "MutationInvalidParent";
        case AuraErrorKind::MutationInvalidField:
            return "MutationInvalidField";
        case AuraErrorKind::MutationUnknownStructuralOp:
            return "MutationUnknownStructuralOp";
        case AuraErrorKind::MutationOutOfRange:
            return "MutationOutOfRange";
        case AuraErrorKind::ArenaOutOfMemory:
            return "ArenaOutOfMemory";
        case AuraErrorKind::ArenaDefragFailed:
            return "ArenaDefragFailed";
        case AuraErrorKind::ArenaInvalidAllocator:
            return "ArenaInvalidAllocator";
        case AuraErrorKind::EvalError:
            return "EvalError";
        case AuraErrorKind::EvalTypeMismatch:
            return "EvalTypeMismatch";
        case AuraErrorKind::EvalDivisionByZero:
            return "EvalDivisionByZero";
        case AuraErrorKind::EvalStackOverflow:
            return "EvalStackOverflow";
        case AuraErrorKind::ConcurrencyFiberCanceled:
            return "ConcurrencyFiberCanceled";
        case AuraErrorKind::ConcurrencyLockFailed:
            return "ConcurrencyLockFailed";
        case AuraErrorKind::ConcurrencyGenerationInvalidated:
            return "ConcurrencyGenerationInvalidated";
        case AuraErrorKind::InternalInvariantViolation:
            return "InternalInvariantViolation";
        case AuraErrorKind::InternalNotImplemented:
            return "InternalNotImplemented";
        case AuraErrorKind::InternalContractFailure:
            return "InternalContractFailure";
        case AuraErrorKind::ResourceQuotaExceeded:
            return "ResourceQuotaExceeded";
        case AuraErrorKind::Sentinel_COUNT_:
            return "Sentinel_COUNT_";
    }
    return "UnknownErrorKind";
}

inline std::unexpected<AuraError> make_unexpected(AuraErrorKind k, std::string msg,
                                                  std::source_location loc, std::uint64_t gen) {
    return std::unexpected<AuraError>(AuraError{k, std::move(msg), loc, gen});
}

inline AuraErrorKind map_kind_name(std::string_view name) noexcept {
    // Keep this table small and stable — used by #807/#808 interop.
    if (name == "parse error" || name == "ParseError")
        return AuraErrorKind::ParseError;
    if (name == "unexpected token" || name == "UnexpectedToken")
        return AuraErrorKind::UnexpectedToken;
    if (name == "unterminated s-expr" || name == "UnterminatedSExpr")
        return AuraErrorKind::UnterminatedSExpr;
    if (name == "unbound variable" || name == "UnboundVariable")
        return AuraErrorKind::UnboundVariable;
    if (name == "division by zero" || name == "DivisionByZero")
        return AuraErrorKind::DivisionByZero;
    if (name == "invalid closure" || name == "InvalidClosure")
        return AuraErrorKind::InvalidClosure;
    if (name == "arity mismatch" || name == "ArityMismatch")
        return AuraErrorKind::ArityMismatch;
    if (name == "type error" || name == "TypeError")
        return AuraErrorKind::TypeError;
    if (name == "out of memory" || name == "OutOfMemory")
        return AuraErrorKind::ArenaOutOfMemory;
    if (name == "internal error" || name == "InternalError")
        return AuraErrorKind::InternalInvariantViolation;
    if (name == "uncaught exception" || name == "UncaughtException")
        return AuraErrorKind::EvalError;
    return AuraErrorKind::EvalError;
}

inline std::unexpected<AuraError> make_unexpected_from_kind_name(std::string_view kind_name,
                                                                 std::string msg,
                                                                 std::source_location loc,
                                                                 std::uint64_t gen) {
    return make_unexpected(map_kind_name(kind_name), std::move(msg), loc, gen);
}

} // namespace aura::core
