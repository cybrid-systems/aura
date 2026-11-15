// src/core/error.ixx — Issue #474: Aura unified error type
// (Phase 0 foundation, scope-limited close).
//
// Purpose: AuraError / AuraResult foundation for explicit failure paths
// Layer: Core
// Safety Class: P0 (error model; no silent exception across fibers)
// Issue: #474 / #807 / #809 / #1886
// AI-Native Rationale: agents need typed failures (kind + message) for
//   self-mod control loops; exceptions break fiber / COW / generation chains
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
//   1. AuraErrorKind — flat enum of the categories core-owned
//      APIs return (mutation, arena, fiber, quota, plus the
//      compiler categories that have a core equivalent). It
//      does NOT replace compiler::diag::ErrorKind: diag is the
//      diagnostic wire taxonomy (kind_ident / JSON code / LSP
//      code), AuraErrorKind is the in-process AuraResult tag.
//      The only conversion is CompilerService::core_kind_of, a
//      switch on the enum — no name or message parsing (#5241).
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
// Nothing migrates wholesale any more: hot paths keep EvalResult
// (std::expected<T, Diagnostic>), and AuraResult is the shape for
// new core-facing APIs. Both are std::expected, so a boundary
// converts without throwing.
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
#include "reflect/error_kind_names.hh"

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
// The Diagnostic → AuraError conversion lives at the compiler boundary
// (CompilerService::core_kind_of): a switch on diag::ErrorKind, because
// Diagnostic is aura.diag and core must not import it. There is no
// kind-name table any more — #5241 removed the unused bridge header and
// the phrase parsing that went with it.
//
// Policy (see docs/design/core/exception_policy.md):
//   - Hot paths return AuraResult / VoidResult (or EvalResult during
//     migration). Exceptions only for OOM / init / true invariants.
export [[nodiscard]] inline AuraError
make_aura_error(AuraErrorKind k, std::string msg,
                std::source_location loc = std::source_location::current(), std::uint64_t gen = 0) {
    return AuraError{k, std::move(msg), loc, gen};
}

} // namespace aura::core

// ═══════════════════════════════════════════════════════════════
// Implementation — kept after the export block so the
// interface is fully visible to consumers.
// ═══════════════════════════════════════════════════════════════

namespace aura::core {

inline std::string_view AuraError::kind_name(AuraErrorKind k) noexcept {
    // Wave C1 wire: table lookup (kAuraErrorKindNames); P2996-aligned in tests.
    return aura_error_kind_name(static_cast<std::size_t>(k));
}

inline std::unexpected<AuraError> make_unexpected(AuraErrorKind k, std::string msg,
                                                  std::source_location loc, std::uint64_t gen) {
    return std::unexpected<AuraError>(AuraError{k, std::move(msg), loc, gen});
}

} // namespace aura::core
