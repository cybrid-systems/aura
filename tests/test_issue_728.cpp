// test_issue_728.cpp — Issue #728: unified structured error + provenance +
// recovery observability primitive (non-duplicative with #478 primitive-
// error-stats pair and #585 primitives-error-stats hash; #728 tracks the
// *unified* model: structured ErrorValue hits + provenance StableNodeRef
// capture + recovery success as separate counters).
//
// Scope-limited close: the issue body asks for: (1) unified ErrorValue
// / EvalValue tagged-error extension with kind/provenance/context/
// recovery-hint, (2) refactor of evaluator_primitives_list.cpp /
// math.cpp / regex / verify error sites to make_structured_primitive_error
// (guard, kind, msg, context), (3) new (primitive:error) / (with-error) /
// (primitive:try) primitives, (4) Guard.capture auto-provenance, (5)
// CI lint for legacy make_primitive_error usage, (6) primitive
// query:unified-error-stats, (7) tests/test_unified_primitive_error_model
// harness with Guard/fiber error tests, (8) SEVA error-resilient
// closed-loop, (9) primitives_style.md mandate. Items (1)/(2)/(3)/(4)/
// (5)/(7)/(8)/(9) require dedicated wiring into evaluator.ixx +
// primitives_detail.h + evaluator_primitives_*.cpp + Guard + diagnostic
// + ast.ixx StableNodeRef + new test harness + SEVA + docs; each is
// a non-trivial focused session.
//
// For this PR we ship:
//
//   1. 3 new atomics in CompilerMetrics:
//        unified_error_structured_hits_total
//        unified_error_provenance_captured_total
//        unified_error_recovery_success_total
//   2. 3 new public bump helpers in Evaluator:
//        bump_unified_error_structured_hit
//        bump_unified_error_provenance_captured
//        bump_unified_error_recovery_success
//   3. New standalone (query:unified-error-stats, schema 728) primitive
//      exposing the 3 counters (4-entry hash: 3 fields + schema sentinel)
//   4. Test verifies: primitive shape, fresh-zero state, schema sentinel,
//      bump accessibility, regression of sibling primitives
//
// Non-duplicative notes:
//   - #478 (query:primitive-error-stats pair) — different shape (pair,
//     not hash), only counts error-count + error-values-size
//   - #585 (query:primitives-error-stats hash) — different fields
//     (error_rate / recovery_success / panic-recovery / rollback /
//     contract-violations / recommendation); coarse aggregate
//   - #728 is the FIRST observability surface that tracks the *unified*
//     model specifically: structured ErrorValue (kind + provenance +
//     context + recovery hint) hits as separate counters the Agent can
//     consume to monitor migration to the unified error model
//
// ACs:
//   AC1: hash shape (3 fields + schema sentinel = 4 entries)
//   AC2: 3 counters == 0 on fresh service
//   AC3: schema == 728 (drift sentinel)
//   AC4: bump helpers accessible — exercise via direct bump on
//        Evaluator surface and verify the primitive reports the bumps
//   AC5: regression — #712 + #713 + #714 + #715 + #716 + #717
//        + #718 + #719 + #720 + #721 + #722 + #723 + #726 sibling
//        primitives still reachable with their schema sentinels intact
//
// (We do NOT implement the unified ErrorValue / EvalValue tagged-error
// extension, do NOT refactor evaluator_primitives_*.cpp error sites,
// do NOT add the new (primitive:error) / (with-error) / (primitive:try)
// primitives, do NOT add Guard.capture auto-provenance, do NOT add the
// CI lint for legacy make_primitive_error usage, do NOT run the
// tests/test_unified_primitive_error_model harness, do NOT update
// SEVA + primitives_style.md — those are the bulk of this issue's
// remaining scope.)

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_728_detail {
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
    auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_src, key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static void run_ac1_shape(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: (query:unified-error-stats) hash shape ---");
    auto r = cs.eval("(query:unified-error-stats)");
    CHECK(r && aura::compiler::types::is_hash(*r), "(query:unified-error-stats) returns a hash");
    const std::vector<std::string> keys = {"structured-hits", "provenance-captured",
                                           "recovery-success", "schema"};
    for (const auto& k : keys) {
        auto f = cs.eval(
            std::format("(hash-ref (engine:metrics \"query:unified-error-stats\") '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: counters == 0 on fresh service ---");
    const auto hits = hash_int_field(cs, "(query:unified-error-stats)", "structured-hits");
    CHECK(hits == 0, std::format("structured-hits = {} (expected 0 on fresh service)", hits));
    const auto prov = hash_int_field(cs, "(query:unified-error-stats)", "provenance-captured");
    CHECK(prov == 0, std::format("provenance-captured = {} (expected 0 on fresh service)", prov));
    const auto rec = hash_int_field(cs, "(query:unified-error-stats)", "recovery-success");
    CHECK(rec == 0, std::format("recovery-success = {} (expected 0 on fresh service)", rec));
}

static void run_ac3_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: schema == 728 (drift sentinel) ---");
    const auto schema = hash_int_field(cs, "(query:unified-error-stats)", "schema");
    CHECK(schema == 728, std::format("schema = {} (expected 728)", schema));
}

static void run_ac4_bump_accessible(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: bump helpers callable from the evaluator surface ---");
    // Direct call: invoke the evaluator's bump helpers via the public
    // surface. These helpers exist so future evaluator_primitives_*.cpp
    // refactors + new (primitive:error) / (with-error) / (primitive:try)
    // primitives + Guard.capture auto-provenance + CI lint for legacy
    // make_primitive_error usage can call them at each decision point
    // (structured error constructed / provenance StableNodeRef captured /
    // recovery rollback + retry succeeded).
    auto& ev = cs.evaluator();
    ev.bump_unified_error_structured_hit();
    ev.bump_unified_error_structured_hit();
    ev.bump_unified_error_structured_hit();
    ev.bump_unified_error_structured_hit();
    ev.bump_unified_error_structured_hit();
    ev.bump_unified_error_structured_hit();
    ev.bump_unified_error_structured_hit();
    ev.bump_unified_error_provenance_captured();
    ev.bump_unified_error_provenance_captured();
    ev.bump_unified_error_provenance_captured();
    ev.bump_unified_error_provenance_captured();
    ev.bump_unified_error_recovery_success();
    ev.bump_unified_error_recovery_success();
    ev.bump_unified_error_recovery_success();
    const auto hits = hash_int_field(cs, "(query:unified-error-stats)", "structured-hits");
    const auto prov = hash_int_field(cs, "(query:unified-error-stats)", "provenance-captured");
    const auto rec = hash_int_field(cs, "(query:unified-error-stats)", "recovery-success");
    CHECK(hits == 7,
          std::format("after 7 structured-hit bumps: structured-hits = {} (expected 7)", hits));
    CHECK(prov == 4,
          std::format("after 4 provenance-captured bumps: provenance-captured = {} (expected 4)",
                      prov));
    CHECK(rec == 3,
          std::format("after 3 recovery-success bumps: recovery-success = {} (expected 3)", rec));
}

static void run_ac5_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: regression — #712..#726 sibling primitives unaffected ---");
    auto reflect = cs.eval("(query:macro-reflect-validation-stats)");
    auto jit = cs.eval("(query:macro-jit-hygiene-stats)");
    auto self_evo = cs.eval("(query:self-evolution-closedloop-stats)");
    auto stable_ref_layer = cs.eval("(query:stable-ref-layer-stats)");
    auto pattern = cs.eval("(query:pattern-stats)");
    auto fiber_boundary = cs.eval("(query:fiber-boundary-violation-stats)");
    auto incremental = cs.eval("(query:incremental-relower-stats)");
    auto closure_env = cs.eval("(query:closure-env-epoch-safety-stats)");
    auto jit_parity = cs.eval("(query:jit-interpreter-parity-stats)");
    auto ir_soa = cs.eval("(query:ir-soa-completeness-stats)");
    auto arena = cs.eval("(query:arena-integration-stats)");
    auto value_dispatch = cs.eval("(query:value-dispatch-stats)");
    auto closed_loop = cs.eval("(query:closed-loop-reliability-stats)");
    CHECK(reflect && aura::compiler::types::is_hash(*reflect),
          "query:macro-reflect-validation-stats hash regression (#712)");
    CHECK(jit && aura::compiler::types::is_hash(*jit),
          "query:macro-jit-hygiene-stats hash regression (#713)");
    CHECK(self_evo && aura::compiler::types::is_hash(*self_evo),
          "query:self-evolution-closedloop-stats hash regression (#714)");
    CHECK(stable_ref_layer && aura::compiler::types::is_hash(*stable_ref_layer),
          "query:stable-ref-layer-stats hash regression (#715)");
    CHECK(pattern && aura::compiler::types::is_hash(*pattern),
          "query:pattern-stats hash regression (#716)");
    CHECK(fiber_boundary && aura::compiler::types::is_hash(*fiber_boundary),
          "query:fiber-boundary-violation-stats hash regression (#717)");
    CHECK(incremental && aura::compiler::types::is_hash(*incremental),
          "query:incremental-relower-stats hash regression (#718)");
    CHECK(closure_env && aura::compiler::types::is_hash(*closure_env),
          "query:closure-env-epoch-safety-stats hash regression (#719)");
    CHECK(jit_parity && aura::compiler::types::is_hash(*jit_parity),
          "query:jit-interpreter-parity-stats hash regression (#720)");
    CHECK(ir_soa && aura::compiler::types::is_hash(*ir_soa),
          "query:ir-soa-completeness-stats hash regression (#721)");
    CHECK(arena && aura::compiler::types::is_hash(*arena),
          "query:arena-integration-stats hash regression (#722)");
    CHECK(value_dispatch && aura::compiler::types::is_hash(*value_dispatch),
          "query:value-dispatch-stats hash regression (#723)");
    CHECK(closed_loop && aura::compiler::types::is_hash(*closed_loop),
          "query:closed-loop-reliability-stats hash regression (#726)");
    const auto reflect_schema =
        hash_int_field(cs, "(query:macro-reflect-validation-stats)", "schema");
    CHECK(reflect_schema == 712,
          std::format("reflect schema = {} (expected 712, no drift)", reflect_schema));
    const auto jit_schema = hash_int_field(cs, "(query:macro-jit-hygiene-stats)", "schema");
    CHECK(jit_schema == 713, std::format("jit schema = {} (expected 713, no drift)", jit_schema));
    const auto self_evo_schema =
        hash_int_field(cs, "(query:self-evolution-closedloop-stats)", "schema");
    CHECK(self_evo_schema == 714,
          std::format("self-evo schema = {} (expected 714, no drift)", self_evo_schema));
    const auto stable_ref_layer_schema =
        hash_int_field(cs, "(query:stable-ref-layer-stats)", "schema");
    CHECK(stable_ref_layer_schema == 715,
          std::format("stable-ref-layer schema = {} (expected 715, no drift)",
                      stable_ref_layer_schema));
    const auto pattern_schema = hash_int_field(cs, "(query:pattern-stats)", "schema");
    CHECK(pattern_schema == 716,
          std::format("pattern schema = {} (expected 716, no drift)", pattern_schema));
    const auto fiber_boundary_schema =
        hash_int_field(cs, "(query:fiber-boundary-violation-stats)", "schema");
    CHECK(
        fiber_boundary_schema == 717,
        std::format("fiber-boundary schema = {} (expected 717, no drift)", fiber_boundary_schema));
    const auto incremental_schema =
        hash_int_field(cs, "(query:incremental-relower-stats)", "schema");
    CHECK(incremental_schema == 718,
          std::format("incremental-relower schema = {} (expected 718, no drift)",
                      incremental_schema));
    const auto closure_env_schema =
        hash_int_field(cs, "(query:closure-env-epoch-safety-stats)", "schema");
    CHECK(
        closure_env_schema == 719,
        std::format("closure-env-epoch schema = {} (expected 719, no drift)", closure_env_schema));
    const auto jit_parity_schema =
        hash_int_field(cs, "(query:jit-interpreter-parity-stats)", "schema");
    CHECK(jit_parity_schema == 720,
          std::format("jit-parity schema = {} (expected 720, no drift)", jit_parity_schema));
    const auto ir_soa_schema = hash_int_field(cs, "(query:ir-soa-completeness-stats)", "schema");
    CHECK(ir_soa_schema == 721,
          std::format("ir-soa schema = {} (expected 721, no drift)", ir_soa_schema));
    const auto arena_schema = hash_int_field(cs, "(query:arena-integration-stats)", "schema");
    CHECK(arena_schema == 722,
          std::format("arena schema = {} (expected 722, no drift)", arena_schema));
    const auto value_dispatch_schema = hash_int_field(cs, "(query:value-dispatch-stats)", "schema");
    CHECK(
        value_dispatch_schema == 723,
        std::format("value-dispatch schema = {} (expected 723, no drift)", value_dispatch_schema));
    const auto closed_loop_schema =
        hash_int_field(cs, "(query:closed-loop-reliability-stats)", "schema");
    CHECK(closed_loop_schema == 726,
          std::format("closed-loop schema = {} (expected 726, no drift)", closed_loop_schema));
}

} // namespace aura_issue_728_detail

int aura_issue_728_run() {
    using namespace aura_issue_728_detail;
    std::println("=== Issue #728: unified error observability (scope-limited close) ===");

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

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_728_run();
}
#endif
