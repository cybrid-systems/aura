// test_issue_474.cpp — Issue #474: Aura unified error type
// foundation (Phase 0 scope-limited close).
//
// Verifies the new aura::core::AuraError + AuraResult<T>
// surface compiles, has the expected shape, and the
// conversion helpers work. The actual API migration
// (Phase 1: hot path, Phase 2: upper modules, Phase 3:
// boundary handling) is a follow-up.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <print>
#include <source_location>

import aura.core.error;
import aura.core.mutation;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println("  PASS: {}", msg); } \
    else      { ++g_failed; std::println("  FAIL: {}", msg); } \
} while (0)

#define CHECK_EQ(a, b, msg) do { \
    auto _a = (a); auto _b = (b); \
    if (_a == _b) { ++g_passed; std::println("  PASS: {}  ({} = {})", msg, _a, _b); } \
    else          { ++g_failed; std::println("  FAIL: {}  ({} != {})", msg, _a, _b); } \
} while (0)

// ── AC1: AuraError default construction + field access ────
bool test_aura_error_defaults() {
    std::println("\n--- AC1: AuraError default construction ---");
    aura::core::AuraError err;
    CHECK_EQ(static_cast<int>(err.kind), 30,
             "default kind == InternalInvariantViolation (ordinal 30)");
    CHECK(err.message.empty(), "default message is empty");
    CHECK_EQ(err.generation, 0u, "default generation == 0");
    return true;
}

// ── AC2: AuraError 4-arg construction ─────────────────────
bool test_aura_error_construction() {
    std::println("\n--- AC2: AuraError 4-arg construction ---");
    aura::core::AuraError err(
        aura::core::AuraErrorKind::TypeError,
        "test message",
        std::source_location::current(),
        42u);
    CHECK_EQ(static_cast<int>(err.kind),
             static_cast<int>(aura::core::AuraErrorKind::TypeError),
             "kind == TypeError");
    CHECK_EQ(err.message, std::string{"test message"},
             "message stored correctly");
    CHECK_EQ(err.generation, 42u, "generation == 42");
    return true;
}

// ── AC3: kind_name static + instance method ──────────────
bool test_kind_name() {
    std::println("\n--- AC3: AuraErrorKind::kind_name ---");
    CHECK_EQ(aura::core::AuraError::kind_name(
                 aura::core::AuraErrorKind::TypeError),
             std::string_view{"TypeError"},
             "TypeError name");
    CHECK_EQ(aura::core::AuraError::kind_name(
                 aura::core::AuraErrorKind::MutationNotCommitted),
             std::string_view{"MutationNotCommitted"},
             "MutationNotCommitted name");
    CHECK_EQ(aura::core::AuraError::kind_name(
                 aura::core::AuraErrorKind::ArenaOutOfMemory),
             std::string_view{"ArenaOutOfMemory"},
             "ArenaOutOfMemory name");
    CHECK_EQ(aura::core::AuraError::kind_name(
                 aura::core::AuraErrorKind::InternalInvariantViolation),
             std::string_view{"InternalInvariantViolation"},
             "InternalInvariantViolation name");
    return true;
}

// ── AC4: make_unexpected builder ─────────────────────────
bool test_make_unexpected() {
    std::println("\n--- AC4: make_unexpected builder ---");
    auto r = aura::core::AuraResult<int>{
        aura::core::make_unexpected(
            aura::core::AuraErrorKind::EvalError,
            "eval failed",
            std::source_location::current(),
            7u)};
    CHECK(r.has_value() == false,
          "AuraResult from make_unexpected has no value");
    CHECK_EQ(static_cast<int>(r.error().kind),
             static_cast<int>(aura::core::AuraErrorKind::EvalError),
             "error kind is EvalError");
    CHECK_EQ(r.error().message, std::string{"eval failed"},
             "error message stored");
    CHECK_EQ(r.error().generation, 7u, "error generation == 7");
    return true;
}

// ── AC5: AuraResult<int> success + failure round-trip ─────
bool test_auraresult_int() {
    std::println("\n--- AC5: AuraResult<int> success + failure ---");
    auto ok = aura::core::AuraResult<int>{42};
    CHECK(ok.has_value(), "success has value");
    CHECK_EQ(*ok, 42, "value is 42");

    auto err = aura::core::AuraResult<int>{
        aura::core::make_unexpected(
            aura::core::AuraErrorKind::InternalInvariantViolation,
            std::string{"failed"})};
    CHECK(err.has_value() == false, "failure has no value");
    CHECK_EQ(static_cast<int>(err.error().kind),
             static_cast<int>(aura::core::AuraErrorKind::InternalInvariantViolation),
             "error kind is InternalInvariantViolation");
    return true;
}

// ── AC6: VoidResult construction ─────────────────────────
bool test_void_result() {
    std::println("\n--- AC6: VoidResult construction ---");
    aura::core::VoidResult ok{};
    CHECK(ok.has_value(), "VoidResult default is success");

    aura::core::VoidResult err = aura::core::make_unexpected(
        aura::core::AuraErrorKind::ArenaOutOfMemory, "OOM");
    CHECK(err.has_value() == false, "VoidResult with unexpected has no value");
    return true;
}

// ── AC7: AuraResult monadic ops (.and_then) ──────────────
bool test_auraresult_monadic() {
    std::println("\n--- AC7: AuraResult monadic ops ---");
    // success path
    auto ok = aura::core::AuraResult<int>{10};
    auto transformed = ok.transform([](int x) { return x * 2; });
    CHECK(transformed.has_value(), "transform preserves success");
    CHECK_EQ(*transformed, 20, "transform doubles");

    // failure propagation
    auto bad = aura::core::AuraResult<int>{
        aura::core::make_unexpected(
            aura::core::AuraErrorKind::TypeError, "type mismatch")};
    auto bad_transformed = bad.transform([](int x) { return x * 2; });
    CHECK(bad_transformed.has_value() == false,
          "transform propagates failure");

    // and_then short-circuits on error
    int calls = 0;
    auto and_then_ok = ok.and_then([&](int x) -> aura::core::AuraResult<int> {
        ++calls;
        return aura::core::AuraResult<int>{x + 1};
    });
    CHECK(and_then_ok.has_value(), "and_then preserves success");
    CHECK_EQ(*and_then_ok, 11, "and_then applies fn");
    CHECK_EQ(calls, 1, "and_then called fn once");

    auto and_then_bad = bad.and_then([&](int) -> aura::core::AuraResult<int> {
        ++calls;
        return aura::core::AuraResult<int>{0};
    });
    CHECK(and_then_bad.has_value() == false,
          "and_then short-circuits on error");
    CHECK_EQ(calls, 1, "and_then did NOT call fn on error");
    return true;
}

// ── AC8: MutationError → AuraErrorKind conversion ────────
bool test_mutation_to_aura_kind() {
    std::println("\n--- AC8: MutationError → AuraErrorKind ---");
    using aura::ast::MutationError;
    using aura::core::AuraErrorKind;
    using aura::ast::mutation_error_to_aura_error_kind;

    CHECK_EQ(static_cast<int>(mutation_error_to_aura_error_kind(
                 MutationError::NotCommitted)),
             static_cast<int>(AuraErrorKind::MutationNotCommitted),
             "NotCommitted → MutationNotCommitted");
    CHECK_EQ(static_cast<int>(mutation_error_to_aura_error_kind(
                 MutationError::NoRollbackData)),
             static_cast<int>(AuraErrorKind::MutationNoRollbackData),
             "NoRollbackData → MutationNoRollbackData");
    CHECK_EQ(static_cast<int>(mutation_error_to_aura_error_kind(
                 MutationError::InvalidTarget)),
             static_cast<int>(AuraErrorKind::MutationInvalidTarget),
             "InvalidTarget → MutationInvalidTarget");
    CHECK_EQ(static_cast<int>(mutation_error_to_aura_error_kind(
                 MutationError::InvalidParent)),
             static_cast<int>(AuraErrorKind::MutationInvalidParent),
             "InvalidParent → MutationInvalidParent");
    CHECK_EQ(static_cast<int>(mutation_error_to_aura_error_kind(
                 MutationError::InvalidField)),
             static_cast<int>(AuraErrorKind::MutationInvalidField),
             "InvalidField → MutationInvalidField");
    CHECK_EQ(static_cast<int>(mutation_error_to_aura_error_kind(
                 MutationError::UnknownStructuralOp)),
             static_cast<int>(AuraErrorKind::MutationUnknownStructuralOp),
             "UnknownStructuralOp → MutationUnknownStructuralOp");
    CHECK_EQ(static_cast<int>(mutation_error_to_aura_error_kind(
                 MutationError::OutOfRange)),
             static_cast<int>(AuraErrorKind::MutationOutOfRange),
             "OutOfRange → MutationOutOfRange");
    return true;
}

// ── AC9: AuraErrorKind category count + sentinel ────────
bool test_kind_categories() {
    std::println("\n--- AC9: AuraErrorKind category count ---");
    // Sanity check: all categories we documented are
    // present (kinda spotty — this is more of a build
    // smoke test than a strict equality).
    CHECK_EQ(static_cast<int>(aura::core::AuraErrorKind::ParseError),
             0, "ParseError is enum[0]");
    CHECK_EQ(static_cast<int>(aura::core::AuraErrorKind::TypeError),
             7, "TypeError is enum[7]");
    CHECK_EQ(static_cast<int>(aura::core::AuraErrorKind::MutationNotCommitted),
             13, "MutationNotCommitted is enum[13]");
    return true;
}

// ── AC10: source_location captured at call site ──────────
bool test_source_location() {
    std::println("\n--- AC10: source_location captured ---");
    aura::core::AuraError err(
        aura::core::AuraErrorKind::InternalInvariantViolation,
        "test",
        std::source_location::current(),
        0);
    CHECK(err.location.line() > 0,
          "source_location.line() is captured (>0)");
    return true;
}

int main() {
    std::println("═══ Issue #474 verification tests ═══\n");
    std::println("AC #1: AuraError defaults");
    test_aura_error_defaults();
    std::println("\nAC #2: AuraError construction");
    test_aura_error_construction();
    std::println("\nAC #3: kind_name static");
    test_kind_name();
    std::println("\nAC #4: make_unexpected");
    test_make_unexpected();
    std::println("\nAC #5: AuraResult<int>");
    test_auraresult_int();
    std::println("\nAC #6: VoidResult");
    test_void_result();
    std::println("\nAC #7: monadic ops");
    test_auraresult_monadic();
    std::println("\nAC #8: MutationError → AuraErrorKind");
    test_mutation_to_aura_kind();
    std::println("\nAC #9: kind categories");
    test_kind_categories();
    std::println("\nAC #10: source_location");
    test_source_location();
    std::println("\n════════════════════════════════════════");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}