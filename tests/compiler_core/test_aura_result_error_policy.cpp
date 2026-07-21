// test_aura_result_error_policy.cpp — Issues #807 + #808:
// Exception policy + AuraResult interop for Evaluator/CompilerService.
//
// #807 ACs:
//   - AC1: map_kind_name / make_unexpected_from_kind_name work
//   - AC2: exception policy doc reachable (compile-time presence via kinds)
//   - AC3: hot-path classification: InternalContractFailure kind exists
//
// #808 Phase 1 ACs:
//   - AC4: eval_as_aura_result success path
//   - AC5: eval_as_aura_result failure path (no throw)
//   - AC6: eval_result_to_aura converts Diagnostic errors
//   - AC7: monadic and_then on success
//   - AC8: regression — classic eval() still works

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.core.error;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_807_808_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::AuraErrorKind;
using aura::core::make_unexpected_from_kind_name;
using aura::core::map_kind_name;

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: map_kind_name / make_unexpected_from_kind_name ---");
    CHECK(map_kind_name("type error") == AuraErrorKind::TypeError, "map type error");
    CHECK(map_kind_name("unbound variable") == AuraErrorKind::UnboundVariable, "map unbound");
    CHECK(map_kind_name("not-a-real-kind") == AuraErrorKind::EvalError,
          "unknown kind → EvalError fallback");
    auto u = make_unexpected_from_kind_name("division by zero", "x/0");
    CHECK(u.error().kind == AuraErrorKind::DivisionByZero, "unexpected carries mapped kind");
    CHECK(u.error().message == "x/0", "unexpected carries message");

    std::println("\n--- AC2–AC3: policy kinds present ---");
    CHECK(aura::core::AuraError::kind_name(AuraErrorKind::InternalContractFailure) ==
              "InternalContractFailure",
          "InternalContractFailure kind named");
    CHECK(aura::core::AuraError::kind_name(AuraErrorKind::ConcurrencyGenerationInvalidated) ==
              "ConcurrencyGenerationInvalidated",
          "concurrency generation kind named");

    std::println("\n--- AC4: eval_as_aura_result success ---");
    auto ok = cs.eval_as_aura_result("(+ 1 2)");
    CHECK(ok.has_value(), "eval_as_aura_result success has_value");
    CHECK(is_int(*ok) && as_int(*ok) == 3, "eval_as_aura_result (+ 1 2) == 3");

    std::println("\n--- AC5: eval_as_aura_result failure (no throw) ---");
    bool threw = false;
    try {
        auto bad = cs.eval_as_aura_result("(this-is-definitely-unbound-xyz-805807808)");
        CHECK(!bad.has_value(), "failure path is unexpected");
        if (!bad.has_value()) {
            CHECK(!bad.error().message.empty() ||
                      bad.error().kind != AuraErrorKind::Sentinel_COUNT_,
                  "error has kind/message");
        }
    } catch (...) {
        threw = true;
    }
    CHECK(!threw, "eval_as_aura_result does not throw on failure");

    std::println("\n--- AC6: eval_result_to_aura ---");
    auto classic = cs.eval("(+ 10 20)");
    auto bridged = CompilerService::eval_result_to_aura(classic);
    CHECK(bridged.has_value() && is_int(*bridged) && as_int(*bridged) == 30,
          "eval_result_to_aura preserves success");
    auto classic_bad = cs.eval("(unbound-symbol-for-bridge-test-zzz)");
    auto bridged_bad = CompilerService::eval_result_to_aura(classic_bad);
    CHECK(!bridged_bad.has_value(), "eval_result_to_aura maps Diagnostic failure");

    std::println("\n--- AC7: monadic and_then ---");
    auto chained = cs.eval_as_aura_result("(+ 2 3)").and_then(
        [](aura::compiler::types::EvalValue v) -> aura::core::AuraResult<std::int64_t> {
            if (!is_int(v))
                return aura::core::make_unexpected(AuraErrorKind::EvalTypeMismatch, "not int");
            return as_int(v) * 2;
        });
    CHECK(chained.has_value() && *chained == 10, "and_then multiplies success value");

    std::println("\n--- AC8: classic eval still works ---");
    auto c = cs.eval("(* 6 7)");
    CHECK(c && is_int(*c) && as_int(*c) == 42, "classic EvalResult eval unchanged");
}

} // namespace aura_issue_807_808_detail

int aura_issue_aura_result_error_policy_run() {
    aura::compiler::CompilerService cs;
    aura_issue_807_808_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_aura_result_error_policy_run();
}
#endif
