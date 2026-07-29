// Wire C1 into business: P2996 validates name tables used by
// AuraError::kind_name / diag::kind_name (module-safe headers).

#include <cstdio>
#include <cstdint>
#include <string_view>

#include "reflect/diag_error_kind_names.hh"
#include "reflect/enum_name_table.hh"
#include "reflect/error_kind_names.hh"

namespace {

// Mirrors of production enums (ordinals must match).
enum class AuraErrorKind : std::uint8_t {
    ParseError,
    UnexpectedToken,
    UnterminatedSExpr,
    UnboundVariable,
    DivisionByZero,
    InvalidClosure,
    ArityMismatch,
    TypeError,
    CoercionError,
    OccurrenceTypingError,
    OwnershipError,
    LinearOwnershipError,
    PatternMatchExhaustiveness,
    MutationNotCommitted,
    MutationNoRollbackData,
    MutationInvalidTarget,
    MutationInvalidParent,
    MutationInvalidField,
    MutationUnknownStructuralOp,
    MutationOutOfRange,
    ArenaOutOfMemory,
    ArenaDefragFailed,
    ArenaInvalidAllocator,
    EvalError,
    EvalTypeMismatch,
    EvalDivisionByZero,
    EvalStackOverflow,
    ConcurrencyFiberCanceled,
    ConcurrencyLockFailed,
    ConcurrencyGenerationInvalidated,
    ResourceQuotaExceeded,
    InternalInvariantViolation,
    InternalNotImplemented,
    InternalContractFailure,
    Sentinel_COUNT_,
};

enum class ErrorKind : std::uint8_t {
    ParseError,
    UnexpectedToken,
    UnterminatedSExpr,
    UnboundVariable,
    DivisionByZero,
    InvalidClosure,
    ArityMismatch,
    TypeError,
    IRCorruption,
    IRNoReturn,
    InternalError,
    OutOfMemory,
    UncaughtException,
    Note,
    Warning,
};

enum class BlameParty : std::uint8_t {
    Caller,
    Annotation,
    Implicit,
    System,
    Narrowing,
};

int g_failed = 0;
void check(bool c, const char* msg) {
    if (!c) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

} // namespace

int main() {
    using namespace aura::reflect;
    using aura::core::aura_error_kind_name;
    using aura::core::kAuraErrorKindNameCount;
    using aura::core::kAuraErrorKindNames;
    using aura::diag::diag_error_kind_display;
    using aura::diag::diag_error_kind_ident;
    using aura::diag::kBlamePartyNameCount;
    using aura::diag::kDiagErrorKindDisplay;
    using aura::diag::kDiagErrorKindIdents;
    using aura::diag::kDiagErrorKindNameCount;

    // ── AuraErrorKind ────────────────────────────────────────
    static_assert(enum_count<AuraErrorKind>() == kAuraErrorKindNameCount);
    static_assert(validate_enum<AuraErrorKind>());
    static_assert(enum_name(AuraErrorKind::ParseError) == "ParseError");
    static_assert(enum_name(AuraErrorKind::ResourceQuotaExceeded) == "ResourceQuotaExceeded");
    static_assert(enum_name(AuraErrorKind::Sentinel_COUNT_) == "Sentinel_COUNT_");

    check(enum_count<AuraErrorKind>() == 35, "aura count");
    for (std::size_t i = 0; i < kAuraErrorKindNameCount; ++i) {
        auto n = enum_name<AuraErrorKind>(static_cast<int>(i));
        if (n != kAuraErrorKindNames[i]) {
            std::printf("FAIL: aura[%zu] reflect=%.*s table=%.*s\n", i, (int)n.size(), n.data(),
                        (int)kAuraErrorKindNames[i].size(), kAuraErrorKindNames[i].data());
            ++g_failed;
        }
        // Business lookup
        if (aura_error_kind_name(i) != kAuraErrorKindNames[i]) {
            std::printf("FAIL: aura_error_kind_name[%zu]\n", i);
            ++g_failed;
        }
    }
    check(aura_error_kind_name(999) == "UnknownErrorKind", "aura unknown");

    // ── diag::ErrorKind idents ───────────────────────────────
    static_assert(enum_count<ErrorKind>() == kDiagErrorKindNameCount);
    static_assert(enum_name(ErrorKind::Warning) == "Warning");
    static_assert(enum_name(ErrorKind::IRCorruption) == "IRCorruption");

    check(enum_count<ErrorKind>() == 15, "diag count");
    for (std::size_t i = 0; i < kDiagErrorKindNameCount; ++i) {
        auto n = enum_name<ErrorKind>(static_cast<int>(i));
        if (n != kDiagErrorKindIdents[i]) {
            std::printf("FAIL: diag ident[%zu] reflect=%.*s table=%.*s\n", i, (int)n.size(),
                        n.data(), (int)kDiagErrorKindIdents[i].size(),
                        kDiagErrorKindIdents[i].data());
            ++g_failed;
        }
        if (diag_error_kind_ident(i) != kDiagErrorKindIdents[i])
            ++g_failed, std::printf("FAIL: diag_error_kind_ident[%zu]\n", i);
    }
    // Display phrases (business kind_name)
    check(diag_error_kind_display(0) == "parse error", "display parse");
    check(diag_error_kind_display(8) == "IR corruption", "display IR");
    check(diag_error_kind_display(14) == "warning", "display warning");
    check(diag_error_kind_display(99) == "unknown", "display unknown");
    check(kDiagErrorKindDisplay[0] == "parse error", "table display 0");

    // ── BlameParty ───────────────────────────────────────────
    static_assert(enum_count<BlameParty>() == kBlamePartyNameCount);
    static_assert(enum_name(BlameParty::Narrowing) == "Narrowing");
    check(enum_name(BlameParty::Caller) == "Caller", "blame Caller");
    check(aura::diag::blame_party_name(4) == "Narrowing", "blame table");

    std::printf("test_error_kind_names_wire: %s (failed=%d)\n", g_failed ? "FAIL" : "PASS",
                g_failed);
    return g_failed == 0 ? 0 : 1;
}
