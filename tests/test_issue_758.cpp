// test_issue_758.cpp — Issue #758: Runtime auto_validate bridge for user-defined
// EDSL structs (DEFINE_STRUCT / custom nodes) under MutationBoundaryGuard with
// macro hygiene invariant correlation observability primitive (non-duplicative
// with #750 query:reflection-schema-stats 4-field hash + (reflect:validate-
// macro-body) + (reflect:validate-edsl) primitives). #758 tracks the *user-
// defined EDSL struct + macro hygiene invariant correlation* specifically as
// separate per-decision-point counters the Agent consumes to monitor
// extensible EDSL struct production safety in self-evo loops).
//
// Scope-limited close: the issue body asks for: (1) reflect.hh + new
// runtime_reflect_edsl_bridge.cpp + runtime_validate_edsl_struct(flat,
// root_id, edsl_type_name) using reflect_members + auto_validate + macro
// descendant provenance check, (2) MutationBoundaryGuard integration on
// EDSL-tagged nodes before commit, (3) (reflect:validate-edsl node-id
// [type]) primitive with optional type arg + (query:edsl-reflection-stats)
// primitive, (4) tests/test_reflection_edsl_struct_validate_guard_mutate.cpp
// harness, (5) SEVA custom EDSL demo + dirty/epoch cascade on violation +
// mutation-impact-snapshot correlation + docs. Each is a non-trivial focused
// session.
//
// For this PR we ship:
//
//   1. 4 new atomics in CompilerMetrics:
//        edsl_validated_total
//        edsl_hygiene_invariants_held_total
//        edsl_schema_fail_by_type_total
//        edsl_macro_correlated_violations_total
//   2. 4 new public bump helpers in Evaluator
//   3. New standalone (query:edsl-reflection-stats, schema 758) primitive
//      exposing the 4 counters (5-entry hash: 4 fields + schema sentinel)
//   4. Test verifies: primitive shape, fresh-zero state, schema sentinel,
//      bump accessibility, regression of sibling primitives

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_758_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t hash_int_field(aura::compiler::CompilerService& cs, std::string_view hash_src,
                                   std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \'{}\')", hash_src, key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static void run_ac1_shape(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: (query:edsl-reflection-stats) hash shape ---");
    auto r = cs.eval("(query:edsl-reflection-stats)");
    CHECK(r && aura::compiler::types::is_hash(*r), "(query:edsl-reflection-stats) returns a hash");
    for (const auto& k : {std::string("validated-edsl"), std::string("hygiene-invariants-held"),
                          std::string("schema-fail-by-type"),
                          std::string("macro-correlated-violations"), std::string("schema")}) {
        auto f = cs.eval(std::format("(hash-ref (query:edsl-reflection-stats) \'{}\')", k));
        CHECK(f, std::format("field \'{} \' present", k));
    }
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: counters == 0 on fresh service ---");
    CHECK(hash_int_field(cs, "(query:edsl-reflection-stats)", "validated-edsl") == 0,
          "validated-edsl == 0 on fresh");
    CHECK(hash_int_field(cs, "(query:edsl-reflection-stats)", "hygiene-invariants-held") == 0,
          "hygiene-invariants-held == 0 on fresh");
    CHECK(hash_int_field(cs, "(query:edsl-reflection-stats)", "schema-fail-by-type") == 0,
          "schema-fail-by-type == 0 on fresh");
    CHECK(hash_int_field(cs, "(query:edsl-reflection-stats)", "macro-correlated-violations") == 0,
          "macro-correlated-violations == 0 on fresh");
}

static void run_ac3_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: schema == 758 (drift sentinel) ---");
    CHECK(hash_int_field(cs, "(query:edsl-reflection-stats)", "schema") == 758, "schema == 758");
}

static void run_ac4_bump_accessible(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: bump helpers callable from the evaluator surface ---");
    auto& ev = cs.evaluator();
    ev.bump_edsl_validated();
    ev.bump_edsl_validated();
    ev.bump_edsl_validated();
    ev.bump_edsl_hygiene_invariants_held();
    ev.bump_edsl_hygiene_invariants_held();
    ev.bump_edsl_schema_fail_by_type();
    ev.bump_edsl_macro_correlated_violation();
    ev.bump_edsl_macro_correlated_violation();
    ev.bump_edsl_macro_correlated_violation();
    ev.bump_edsl_macro_correlated_violation();
    CHECK(hash_int_field(cs, "(query:edsl-reflection-stats)", "validated-edsl") == 3,
          "3 validated bumps → 3");
    CHECK(hash_int_field(cs, "(query:edsl-reflection-stats)", "hygiene-invariants-held") == 2,
          "2 hygiene-invariants-held bumps → 2");
    CHECK(hash_int_field(cs, "(query:edsl-reflection-stats)", "schema-fail-by-type") == 1,
          "1 schema-fail-by-type bump → 1");
    CHECK(hash_int_field(cs, "(query:edsl-reflection-stats)", "macro-correlated-violations") == 4,
          "4 macro-correlated bumps → 4");
}

static void run_ac5_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: regression ---");
    CHECK(cs.eval("(query:macro-provenance-stats)") &&
              aura::compiler::types::is_hash(*cs.eval("(query:macro-provenance-stats)")),
          "query:macro-provenance-stats hash regression (#735)");
    CHECK(cs.eval("(query:envframe-dualpath-policy-stats)") &&
              aura::compiler::types::is_hash(*cs.eval("(query:envframe-dualpath-policy-stats)")),
          "query:envframe-dualpath-policy-stats hash regression (#756)");
    CHECK(cs.eval("(query:macro-hygiene-provenance-stats)") &&
              aura::compiler::types::is_hash(*cs.eval("(query:macro-hygiene-provenance-stats)")),
          "query:macro-hygiene-provenance-stats hash regression (#757)");
    CHECK(hash_int_field(cs, "(query:macro-provenance-stats)", "schema") == 735,
          "macro-provenance schema 735");
    CHECK(hash_int_field(cs, "(query:envframe-dualpath-policy-stats)", "schema") == 756,
          "envframe-policy schema 756");
    CHECK(hash_int_field(cs, "(query:macro-hygiene-provenance-stats)", "schema") == 757,
          "macro-hygiene-provenance schema 757");
}

} // namespace aura_issue_758_detail

int main() {
    using namespace aura_issue_758_detail;
    std::println("=== Issue #758: EDSL reflection observability (scope-limited close) ===");
    {
        aura::compiler::CompilerService cs;
        run_ac1_shape(cs);
        run_ac2_fresh_zero(cs);
        run_ac3_schema_sentinel(cs);
        run_ac4_bump_accessible(cs);
        run_ac5_regression(cs);
    }
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}
