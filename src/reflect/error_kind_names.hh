// ──────────────────────────────────────────────────────────────
//  error_kind_names.hh — AuraErrorKind names (meta-free, C1 wire)
//
//  PascalCase identifiers for aura::core::AuraErrorKind; order must
//  match error.ixx (0 .. Sentinel_COUNT_ inclusive).
//  Validated by tests/reflect/test_error_kind_names_wire.cpp.
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_ERROR_KIND_NAMES_HH
#define AURA_REFLECT_ERROR_KIND_NAMES_HH

#include <cstddef>
#include <string_view>

namespace aura::core {

inline constexpr std::size_t kAuraErrorKindNameCount = 35;

inline constexpr std::string_view kAuraErrorKindNames[kAuraErrorKindNameCount] = {
    "ParseError",
    "UnexpectedToken",
    "UnterminatedSExpr",
    "UnboundVariable",
    "DivisionByZero",
    "InvalidClosure",
    "ArityMismatch",
    "TypeError",
    "CoercionError",
    "OccurrenceTypingError",
    "OwnershipError",
    "LinearOwnershipError",
    "PatternMatchExhaustiveness",
    "MutationNotCommitted",
    "MutationNoRollbackData",
    "MutationInvalidTarget",
    "MutationInvalidParent",
    "MutationInvalidField",
    "MutationUnknownStructuralOp",
    "MutationOutOfRange",
    "ArenaOutOfMemory",
    "ArenaDefragFailed",
    "ArenaInvalidAllocator",
    "EvalError",
    "EvalTypeMismatch",
    "EvalDivisionByZero",
    "EvalStackOverflow",
    "ConcurrencyFiberCanceled",
    "ConcurrencyLockFailed",
    "ConcurrencyGenerationInvalidated",
    "ResourceQuotaExceeded",
    "InternalInvariantViolation",
    "InternalNotImplemented",
    "InternalContractFailure",
    "Sentinel_COUNT_",
};

[[nodiscard]] inline std::string_view aura_error_kind_name(std::size_t ordinal) noexcept {
    if (ordinal < kAuraErrorKindNameCount)
        return kAuraErrorKindNames[ordinal];
    return "UnknownErrorKind";
}

} // namespace aura::core

#endif // AURA_REFLECT_ERROR_KIND_NAMES_HH
