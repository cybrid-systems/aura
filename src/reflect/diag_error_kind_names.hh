// ──────────────────────────────────────────────────────────────
//  diag_error_kind_names.hh — diag::ErrorKind / BlameParty (meta-free)
//
//  C1 business wire:
//    kDiagErrorKindIdents  — PascalCase (P2996-aligned)
//    kDiagErrorKindDisplay — human phrases for kind_name()
//    kBlamePartyNames      — PascalCase (format() lowercases)
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_DIAG_ERROR_KIND_NAMES_HH
#define AURA_REFLECT_DIAG_ERROR_KIND_NAMES_HH

#include <cstddef>
#include <string_view>

namespace aura::diag {

inline constexpr std::size_t kDiagErrorKindNameCount = 15;

inline constexpr std::string_view kDiagErrorKindIdents[kDiagErrorKindNameCount] = {
    "ParseError",        "UnexpectedToken",
    "UnterminatedSExpr", "UnboundVariable",
    "DivisionByZero",    "InvalidClosure",
    "ArityMismatch",     "TypeError",
    "IRCorruption",      "IRNoReturn",
    "InternalError",     "OutOfMemory",
    "UncaughtException", "Note",
    "Warning",
};

inline constexpr std::string_view kDiagErrorKindDisplay[kDiagErrorKindNameCount] = {
    "parse error",
    "unexpected token",
    "unterminated s-expr",
    "unbound variable",
    "division by zero",
    "invalid closure",
    "arity mismatch",
    "type error",
    "IR corruption",
    "no return",
    "internal error",
    "out of memory",
    "uncaught exception",
    "note",
    "warning",
};

inline constexpr std::size_t kBlamePartyNameCount = 5;

inline constexpr std::string_view kBlamePartyNames[kBlamePartyNameCount] = {
    "Caller", "Annotation", "Implicit", "System", "Narrowing",
};

[[nodiscard]] inline std::string_view diag_error_kind_display(std::size_t ordinal) noexcept {
    if (ordinal < kDiagErrorKindNameCount)
        return kDiagErrorKindDisplay[ordinal];
    return "unknown";
}

[[nodiscard]] inline std::string_view diag_error_kind_ident(std::size_t ordinal) noexcept {
    if (ordinal < kDiagErrorKindNameCount)
        return kDiagErrorKindIdents[ordinal];
    return "Unknown";
}

[[nodiscard]] inline std::string_view blame_party_name(std::size_t ordinal) noexcept {
    if (ordinal < kBlamePartyNameCount)
        return kBlamePartyNames[ordinal];
    return "Unknown";
}

} // namespace aura::diag

#endif // AURA_REFLECT_DIAG_ERROR_KIND_NAMES_HH
