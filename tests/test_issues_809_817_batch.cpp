// test_issues_809_817_batch.cpp — Phase 1 close for Issues #809–#817.
//
// Observability + policy + light wiring (non-full migration):
//   #809 error-handling-policy-stats + interop bump via eval_as_aura_result
//   #810 fiber-scheduler-init-stats
//   #811 jit-exception-bridge-stats
//   #812 orchestration-steal-arena-gc-stats
//   #813 guard-error-stats
//   #814 runtime-production-health + runtime:self-heal-on-drift
//   #815 macro-introduced-provenance-stats
//   #816 edsl-struct-meta-stats + edsl:define-struct
//   #817 dirty-epoch-marker-stats

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.error;

namespace aura_issues_809_817 {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::AuraErrorKind;
using aura::core::map_kind_name;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref ({}) '{}')", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void check_schema(CompilerService& cs, std::string_view q, std::int64_t schema) {
    auto h = cs.eval(std::format("({})", q));
    CHECK(h && is_hash(*h), std::format("{} returns hash", q));
    CHECK(href(cs, q, "schema") == schema, std::format("{} schema == {}", q, schema));
}

static void run_matrix(CompilerService& cs) {
    auto& ev = cs.evaluator();

    std::println("\n--- #809 error-handling-policy-stats ---");
    check_schema(cs, "query:error-handling-policy-stats", 809);
    CHECK(href(cs, "query:error-handling-policy-stats", "policy-doc-active") == 1,
          "policy-doc-active");
    CHECK(map_kind_name("type error") == AuraErrorKind::TypeError, "map_kind_name still works");
    const auto interop0 = href(cs, "query:error-handling-policy-stats", "interop-conversions");
    (void)cs.eval_as_aura_result("(+ 1 1)");
    CHECK(href(cs, "query:error-handling-policy-stats", "interop-conversions") >= interop0 + 1,
          "eval_as_aura_result bumps interop-conversions");
    ev.bump_error_policy_contract_as_aura_error();
    CHECK(href(cs, "query:error-handling-policy-stats", "contract-as-aura-error") >= 1,
          "contract-as-aura-error bump");

    std::println("\n--- #810 fiber-scheduler-init-stats ---");
    check_schema(cs, "query:fiber-scheduler-init-stats", 810);
    CHECK(href(cs, "query:fiber-scheduler-init-stats", "aura-result-init-active") == 1,
          "aura-result-init-active");
    ev.bump_fiber_init_aura_result_ok();
    ev.bump_scheduler_init_aura_result_ok();
    CHECK(href(cs, "query:fiber-scheduler-init-stats", "fiber-init-ok") >= 1, "fiber-init-ok");
    CHECK(href(cs, "query:fiber-scheduler-init-stats", "scheduler-init-ok") >= 1,
          "scheduler-init-ok");

    std::println("\n--- #811 jit-exception-bridge-stats ---");
    check_schema(cs, "query:jit-exception-bridge-stats", 811);
    CHECK(href(cs, "query:jit-exception-bridge-stats", "guest-only-policy-active") == 1,
          "guest-only-policy-active");
    ev.bump_jit_guest_exception_bridge();
    ev.bump_jit_internal_aura_result_path();
    CHECK(href(cs, "query:jit-exception-bridge-stats", "guest-exception-bridge") >= 1,
          "guest-exception-bridge");
    CHECK(href(cs, "query:jit-exception-bridge-stats", "internal-aura-result-path") >= 1,
          "internal-aura-result-path");

    std::println("\n--- #812 orchestration-steal-arena-gc-stats ---");
    check_schema(cs, "query:orchestration-steal-arena-gc-stats", 812);
    CHECK(href(cs, "query:orchestration-steal-arena-gc-stats", "steal-safety-active") == 1,
          "steal-safety-active");
    ev.bump_steal_arena_yield_during_compact();
    ev.bump_steal_outermost_only_enforced();
    ev.bump_steal_linear_probe_on_success();
    CHECK(href(cs, "query:orchestration-steal-arena-gc-stats", "yield-during-compact") >= 1,
          "yield-during-compact");
    CHECK(href(cs, "query:orchestration-steal-arena-gc-stats", "outermost-only-enforced") >= 1,
          "outermost-only-enforced");
    CHECK(href(cs, "query:orchestration-steal-arena-gc-stats", "linear-probe-on-success") >= 1,
          "linear-probe-on-success");

    std::println("\n--- #813 guard-error-stats ---");
    check_schema(cs, "query:guard-error-stats", 813);
    CHECK(href(cs, "query:guard-error-stats", "no-unwind-through-guard") == 1,
          "no-unwind-through-guard");
    ev.bump_guard_aura_result_path();
    ev.bump_guard_panic_checkpoint_aura_result();
    CHECK(href(cs, "query:guard-error-stats", "guard-aura-result-path") >= 1,
          "guard-aura-result-path");
    CHECK(href(cs, "query:guard-error-stats", "panic-checkpoint-aura-result") >= 1,
          "panic-checkpoint-aura-result");

    std::println("\n--- #814 runtime-production-health + self-heal ---");
    check_schema(cs, "query:runtime-production-health", 814);
    CHECK(href(cs, "query:runtime-production-health", "health-score") == 100,
          "fresh health-score == 100");
    ev.bump_runtime_health_drift_detected(3);
    CHECK(href(cs, "query:runtime-production-health", "drift-violations") >= 3, "drift-violations");
    CHECK(href(cs, "query:runtime-production-health", "recommended-action") == 1,
          "recommended-action when unpaid drift");
    auto heal = cs.eval("(runtime:self-heal-on-drift)");
    CHECK(heal && is_bool(*heal), "self-heal returns bool");
    (void)cs.eval("(runtime:self-heal-on-drift)");
    (void)cs.eval("(runtime:self-heal-on-drift)");
    CHECK(href(cs, "query:runtime-production-health", "self-heal-invocations") >= 3,
          "self-heal-invocations");
    CHECK(href(cs, "query:runtime-production-health", "health-score") == 100,
          "health-score restored after heal matches drift");

    std::println("\n--- #815 macro-introduced-provenance-stats ---");
    check_schema(cs, "query:macro-introduced-provenance-stats", 815);
    CHECK(href(cs, "query:macro-introduced-provenance-stats", "marker-propagation-active") == 1,
          "marker-propagation-active");
    ev.bump_macro_ir_source_marker_stamp();
    ev.bump_macro_provenance_query();
    CHECK(href(cs, "query:macro-introduced-provenance-stats", "ir-source-marker-stamps") >= 1,
          "ir-source-marker-stamps");
    CHECK(href(cs, "query:macro-introduced-provenance-stats", "provenance-queries") >= 1,
          "provenance-queries");

    std::println("\n--- #816 edsl-struct-meta-stats + define-struct ---");
    check_schema(cs, "query:edsl-struct-meta-stats", 816);
    auto ok = cs.eval("(edsl:define-struct \"MyStruct\" \"doc\" \"fields:int\")");
    CHECK(ok && is_bool(*ok), "edsl:define-struct returns bool");
    CHECK(href(cs, "query:edsl-struct-meta-stats", "define-struct-total") >= 1,
          "define-struct-total");
    CHECK(href(cs, "query:edsl-struct-meta-stats", "validate-pass") >= 1, "validate-pass");
    auto bad = cs.eval("(edsl:define-struct)");
    CHECK(bad && is_bool(*bad), "arity-fail define-struct returns bool");
    CHECK(href(cs, "query:edsl-struct-meta-stats", "validate-fail") >= 1, "validate-fail");
    CHECK(href(cs, "query:edsl-struct-meta-stats", "validate-pass-pct") >= 0 &&
              href(cs, "query:edsl-struct-meta-stats", "validate-pass-pct") <= 10000,
          "validate-pass-pct in range");

    std::println("\n--- #817 dirty-epoch-marker-stats ---");
    check_schema(cs, "query:dirty-epoch-marker-stats", 817);
    CHECK(href(cs, "query:dirty-epoch-marker-stats", "marker-aware-dirty-active") == 1,
          "marker-aware-dirty-active");
    ev.bump_dirty_epoch_macro_introduced_hit();
    ev.bump_dirty_epoch_targeted_relower();
    ev.bump_dirty_epoch_hygiene_drift_prevented();
    CHECK(href(cs, "query:dirty-epoch-marker-stats", "macro-introduced-dirty-hits") >= 1,
          "macro-introduced-dirty-hits");
    CHECK(href(cs, "query:dirty-epoch-marker-stats", "targeted-relower") >= 1, "targeted-relower");
    CHECK(href(cs, "query:dirty-epoch-marker-stats", "hygiene-drift-prevented") >= 1,
          "hygiene-drift-prevented");

    std::println("\n--- regression: classic eval + #808 bridge ---");
    auto c = cs.eval("(* 6 7)");
    CHECK(c && is_int(*c) && as_int(*c) == 42, "classic eval (* 6 7) == 42");
    auto ar = cs.eval_as_aura_result("(+ 10 20)");
    CHECK(ar.has_value() && is_int(*ar) && as_int(*ar) == 30, "eval_as_aura_result still works");
}

} // namespace aura_issues_809_817

int main() {
    aura::compiler::CompilerService cs;
    aura_issues_809_817::run_matrix(cs);
    return RUN_ALL_TESTS();
}
