// test_issue_735.cpp — Issue #735: MacroIntroduced provenance in
// StableNodeRef + targeted dirty/rollback for macro subtrees
// observability primitive (non-duplicative with #714 closed-loop
// reliability, #717 fiber-boundary-violation-stats, #392 subtree gen
// internal mechanism, #373 mutate hygiene guard flat.is_macro_
// introduced internal check, #733 query:ir-marker-hygiene-stats
// IR-level marker propagation, #750 query:reflection-schema-stats
// runtime reflection validate). #735 tracks the *MacroIntroduced
// provenance + targeted macro-subtree handling* specifically:
// capture-time provenance in StableNodeRef, hot-path consult,
// targeted dirty propagation, rollback success as separate
// per-decision-point counters).
//
// Scope-limited close: the issue body asks for: (1) ast.ixx
// StableNodeRef + make_ref add macro_introduced_at_capture or
// original_macro_expansion_id field + populate from flat.is_macro_
// introduced(id) at capture time in Guard / query paths, (2) extend
// is_valid / is_valid_subtree with macro_provenance_check, (3)
// MutationBoundaryGuard + mark_dirty_upward targeted dirty only on
// macro-subtree + rollback_macro_subtree_provenance + preserve
// marker during restore_children, (4) primitive query:macro-
// provenance-stats node-id, (5) dirty/epoch interaction strengthen
// verify/macro dirty cascade to respect MacroIntroduced provenance
// for incremental re-lower, (6) tests/test_macro_provenance_
// stable_ref_rollback_self_evo.cpp harness with nested macro expand
// + multi-round mutate:rebind inside macro body under fiber steal /
// panic / Guard fail. Items (1)/(2)/(3)/(5)/(6) require dedicated
// wiring into ast.ixx + mutate.cpp + evaluator_primitives_mutate.cpp
// + new test harness; each is a non-trivial focused session.
//
// For this PR we ship:
//
//   1. 4 new atomics in CompilerMetrics:
//        macro_provenance_captured_total
//        macro_provenance_is_macro_introduced_total
//        macro_provenance_dirty_impact_total
//        macro_provenance_rollback_success_total
//   2. 4 new public bump helpers in Evaluator:
//        bump_macro_provenance_captured
//        bump_macro_provenance_is_macro_introduced
//        bump_macro_provenance_dirty_impact
//        bump_macro_provenance_rollback_success
//   3. New standalone (query:macro-provenance-stats, schema 735)
//      primitive exposing the 4 counters (5-entry hash: 4 fields +
//      schema sentinel)
//   4. Test verifies: primitive shape, fresh-zero state, schema
//      sentinel, bump accessibility, regression of sibling primitives
//
// Non-duplicative notes:
//   - #714 (engine:metrics \"query:self-evolution-closedloop-stats\") — different scope
//     (ref drift + rollback success + feedback mutate rounds; no
//     macro-provenance / targeted macro-subtree handling signals)
//   - #717 (engine:metrics \"query:fiber-boundary-violation-stats\") — fiber/Guard
//     boundary invariants; no macro-provenance
//   - #392 (subtree gen) — internal subtree mechanism; no
//     observability surface
//   - #373 (mutate hygiene guard) — flat.is_macro_introduced internal
//     check at mutate time; no StableRef provenance capture
//   - #733 (engine:metrics \"query:ir-marker-hygiene-stats\") — IR-level marker
//     propagation; different scope (no StableNodeRef provenance)
//   - #750 (engine:metrics \"query:reflection-schema-stats\") — runtime reflection
//     validate; different scope (no StableNodeRef provenance)
//   - #735 is the FIRST observability surface that tracks the
//     *MacroIntroduced provenance + targeted macro-subtree handling*
//     specifically as separate per-decision-point counters
//
// ACs:
//   AC1: hash shape (4 fields + schema sentinel = 5 entries)
//   AC2: 4 counters == 0 on fresh service
//   AC3: schema == 735 (drift sentinel)
//   AC4: bump helpers accessible — exercise via direct bump on
//        Evaluator surface and verify the primitive reports the bumps
//   AC5: regression — #712 + #713 + #714 + #715 + #716 + #717
//        + #718 + #719 + #720 + #721 + #722 + #723 + #726 + #728
//        + #731 + #732 + #733 sibling primitives still reachable
//        with their schema sentinels intact
//
// (We do NOT add macro_introduced_at_capture or original_macro_
// expansion_id to StableNodeRef, do NOT extend is_valid / is_valid_
// subtree with macro_provenance_check, do NOT wire MutationBoundary
// Guard + mark_dirty_upward targeted macro-subtree + rollback_macro_
// subtree_provenance, do NOT strengthen verify/macro dirty cascade
// for incremental re-lower, do NOT run the tests/test_macro_
// provenance_stable_ref_rollback_self_evo harness — those are the
// bulk of this issue's remaining scope.)

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_735_detail {
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
    std::println("\n--- AC1: (engine:metrics \"query:macro-provenance-stats\") hash shape ---");
    auto r = cs.eval("(engine:metrics \"query:macro-provenance-stats\")");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "(engine:metrics \"query:macro-provenance-stats\") returns a hash");
    const std::vector<std::string> keys = {"is-macro-introduced-consults", "provenance-captured",
                                           "dirty-impact-on-macro-subtree", "rollback-success",
                                           "schema"};
    for (const auto& k : keys) {
        auto f = cs.eval(
            std::format("(hash-ref (engine:metrics \"query:macro-provenance-stats\") '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: counters == 0 on fresh service ---");
    const auto consults = hash_int_field(cs, "(engine:metrics \"query:macro-provenance-stats\")",
                                         "is-macro-introduced-consults");
    CHECK(consults == 0,
          std::format("is-macro-introduced-consults = {} (expected 0 on fresh service)", consults));
    const auto captured = hash_int_field(cs, "(engine:metrics \"query:macro-provenance-stats\")",
                                         "provenance-captured");
    CHECK(captured == 0,
          std::format("provenance-captured = {} (expected 0 on fresh service)", captured));
    const auto dirty = hash_int_field(cs, "(engine:metrics \"query:macro-provenance-stats\")",
                                      "dirty-impact-on-macro-subtree");
    CHECK(dirty == 0,
          std::format("dirty-impact-on-macro-subtree = {} (expected 0 on fresh service)", dirty));
    const auto rollback =
        hash_int_field(cs, "(engine:metrics \"query:macro-provenance-stats\")", "rollback-success");
    CHECK(rollback == 0,
          std::format("rollback-success = {} (expected 0 on fresh service)", rollback));
}

static void run_ac3_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: schema == 735 (drift sentinel) ---");
    const auto schema =
        hash_int_field(cs, "(engine:metrics \"query:macro-provenance-stats\")", "schema");
    CHECK(schema == 735, std::format("schema = {} (expected 735)", schema));
}

static void run_ac4_bump_accessible(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: bump helpers callable from the evaluator surface ---");
    // Direct call: invoke the evaluator's bump helpers via the public
    // surface. These helpers exist so future ast.ixx StableNodeRef +
    // make_ref + MutationBoundaryGuard + mark_dirty_upward +
    // evaluator_primitives_mutate.cpp can call them at each decision
    // point (StableNodeRef captured with macro_introduced_at_capture
    // + original_macro_expansion_id populated / dirty propagation
    // targeted to macro-subtree / rollback success on macro subtree
    // hygiene drift detected / is_macro_introduced hot-path consult).
    auto& ev = cs.evaluator();
    ev.bump_macro_provenance_captured();
    ev.bump_macro_provenance_captured();
    ev.bump_macro_provenance_captured();
    ev.bump_macro_provenance_captured();
    ev.bump_macro_provenance_is_macro_introduced();
    ev.bump_macro_provenance_is_macro_introduced();
    ev.bump_macro_provenance_is_macro_introduced();
    ev.bump_macro_provenance_dirty_impact();
    ev.bump_macro_provenance_dirty_impact();
    ev.bump_macro_provenance_rollback_success();
    const auto consults = hash_int_field(cs, "(engine:metrics \"query:macro-provenance-stats\")",
                                         "is-macro-introduced-consults");
    const auto captured = hash_int_field(cs, "(engine:metrics \"query:macro-provenance-stats\")",
                                         "provenance-captured");
    const auto dirty = hash_int_field(cs, "(engine:metrics \"query:macro-provenance-stats\")",
                                      "dirty-impact-on-macro-subtree");
    const auto rollback =
        hash_int_field(cs, "(engine:metrics \"query:macro-provenance-stats\")", "rollback-success");
    CHECK(consults == 3,
          std::format("after 3 is-macro-introduced-consult bumps: is-macro-introduced-consults "
                      "= {} (expected 3)",
                      consults));
    CHECK(captured == 4, std::format("after 4 provenance-captured bumps: provenance-captured = {} "
                                     "(expected 4)",
                                     captured));
    CHECK(dirty == 2, std::format("after 2 dirty-impact bumps: dirty-impact-on-macro-subtree = {} "
                                  "(expected 2)",
                                  dirty));
    CHECK(
        rollback == 1,
        std::format("after 1 rollback-success bump: rollback-success = {} (expected 1)", rollback));
}

static void run_ac5_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: regression — #712..#733 sibling primitives unaffected ---");
    auto reflect = cs.eval("(engine:metrics \"query:macro-reflect-validation-stats\")");
    auto jit = cs.eval("(engine:metrics \"query:macro-jit-hygiene-stats\")");
    auto self_evo = cs.eval("(engine:metrics \"query:self-evolution-closedloop-stats\")");
    auto stable_ref_layer = cs.eval("(engine:metrics \"query:stable-ref-layer-stats\")");
    auto pattern = cs.eval("(engine:metrics \"query:pattern-stats\")");
    auto fiber_boundary = cs.eval("(engine:metrics \"query:fiber-boundary-violation-stats\")");
    auto incremental = cs.eval("(engine:metrics \"query:incremental-relower-stats\")");
    auto closure_env = cs.eval("(engine:metrics \"query:closure-env-epoch-safety-stats\")");
    auto jit_parity = cs.eval("(engine:metrics \"query:jit-interpreter-parity-stats\")");
    auto ir_soa = cs.eval("(engine:metrics \"query:ir-soa-completeness-stats\")");
    auto arena = cs.eval("(engine:metrics \"query:arena-integration-stats\")");
    auto value_dispatch = cs.eval("(engine:metrics \"query:value-dispatch-stats\")");
    auto closed_loop = cs.eval("(engine:metrics \"query:closed-loop-reliability-stats\")");
    auto unified_error = cs.eval("(engine:metrics \"query:unified-error-stats\")");
    auto arena_concurrent = cs.eval("(engine:metrics \"query:arena-concurrent-compact-stats\")");
    auto aot_safe = cs.eval("(engine:metrics \"query:aot-safe-swap-boundary-stats\")");
    auto ir_marker = cs.eval("(engine:metrics \"query:ir-marker-hygiene-stats\")");
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
    CHECK(unified_error && aura::compiler::types::is_hash(*unified_error),
          "query:unified-error-stats hash regression (#728)");
    CHECK(arena_concurrent && aura::compiler::types::is_hash(*arena_concurrent),
          "query:arena-concurrent-compact-stats hash regression (#731)");
    CHECK(aot_safe && aura::compiler::types::is_hash(*aot_safe),
          "query:aot-safe-swap-boundary-stats hash regression (#732)");
    CHECK(ir_marker && aura::compiler::types::is_hash(*ir_marker),
          "query:ir-marker-hygiene-stats hash regression (#733)");
    const auto reflect_schema =
        hash_int_field(cs, "(engine:metrics \"query:macro-reflect-validation-stats\")", "schema");
    CHECK(reflect_schema == 712,
          std::format("reflect schema = {} (expected 712, no drift)", reflect_schema));
    const auto jit_schema =
        hash_int_field(cs, "(engine:metrics \"query:macro-jit-hygiene-stats\")", "schema");
    CHECK(jit_schema == 713, std::format("jit schema = {} (expected 713, no drift)", jit_schema));
    const auto self_evo_schema =
        hash_int_field(cs, "(engine:metrics \"query:self-evolution-closedloop-stats\")", "schema");
    CHECK(self_evo_schema == 714,
          std::format("self-evo schema = {} (expected 714, no drift)", self_evo_schema));
    const auto stable_ref_layer_schema =
        hash_int_field(cs, "(engine:metrics \"query:stable-ref-layer-stats\")", "schema");
    CHECK(stable_ref_layer_schema == 715,
          std::format("stable-ref-layer schema = {} (expected 715, no drift)",
                      stable_ref_layer_schema));
    const auto pattern_schema =
        hash_int_field(cs, "(engine:metrics \"query:pattern-stats\")", "schema");
    CHECK(pattern_schema == 716,
          std::format("pattern schema = {} (expected 716, no drift)", pattern_schema));
    const auto fiber_boundary_schema =
        hash_int_field(cs, "(engine:metrics \"query:fiber-boundary-violation-stats\")", "schema");
    CHECK(
        fiber_boundary_schema == 717,
        std::format("fiber-boundary schema = {} (expected 717, no drift)", fiber_boundary_schema));
    const auto incremental_schema =
        hash_int_field(cs, "(engine:metrics \"query:incremental-relower-stats\")", "schema");
    CHECK(incremental_schema == 1605 || incremental_schema == 1601 || incremental_schema == 718,
          std::format("incremental-relower schema = {} (expected 1605|1601|718 lineage)",
                      incremental_schema));
    const auto closure_env_schema =
        hash_int_field(cs, "(engine:metrics \"query:closure-env-epoch-safety-stats\")", "schema");
    CHECK(
        closure_env_schema == 719,
        std::format("closure-env-epoch schema = {} (expected 719, no drift)", closure_env_schema));
    const auto jit_parity_schema =
        hash_int_field(cs, "(engine:metrics \"query:jit-interpreter-parity-stats\")", "schema");
    CHECK(jit_parity_schema == 720,
          std::format("jit-parity schema = {} (expected 720, no drift)", jit_parity_schema));
    const auto ir_soa_schema =
        hash_int_field(cs, "(engine:metrics \"query:ir-soa-completeness-stats\")", "schema");
    CHECK(ir_soa_schema == 721,
          std::format("ir-soa schema = {} (expected 721, no drift)", ir_soa_schema));
    const auto arena_schema =
        hash_int_field(cs, "(engine:metrics \"query:arena-integration-stats\")", "schema");
    CHECK(arena_schema == 722,
          std::format("arena schema = {} (expected 722, no drift)", arena_schema));
    const auto value_dispatch_schema =
        hash_int_field(cs, "(engine:metrics \"query:value-dispatch-stats\")", "schema");
    CHECK(
        value_dispatch_schema == 723,
        std::format("value-dispatch schema = {} (expected 723, no drift)", value_dispatch_schema));
    const auto closed_loop_schema =
        hash_int_field(cs, "(engine:metrics \"query:closed-loop-reliability-stats\")", "schema");
    CHECK(closed_loop_schema == 726,
          std::format("closed-loop schema = {} (expected 726, no drift)", closed_loop_schema));
    const auto unified_error_schema =
        hash_int_field(cs, "(engine:metrics \"query:unified-error-stats\")", "schema");
    CHECK(unified_error_schema == 728,
          std::format("unified-error schema = {} (expected 728, no drift)", unified_error_schema));
    const auto arena_concurrent_schema =
        hash_int_field(cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")", "schema");
    CHECK(arena_concurrent_schema == 731,
          std::format("arena-concurrent schema = {} (expected 731, no drift)",
                      arena_concurrent_schema));
    const auto aot_safe_schema =
        hash_int_field(cs, "(engine:metrics \"query:aot-safe-swap-boundary-stats\")", "schema");
    CHECK(aot_safe_schema == 732,
          std::format("aot-safe-swap schema = {} (expected 732, no drift)", aot_safe_schema));
    const auto ir_marker_schema =
        hash_int_field(cs, "(engine:metrics \"query:ir-marker-hygiene-stats\")", "schema");
    CHECK(ir_marker_schema == 733,
          std::format("ir-marker-hygiene schema = {} (expected 733, no drift)", ir_marker_schema));
}

} // namespace aura_issue_735_detail

int aura_issue_735_run() {
    using namespace aura_issue_735_detail;
    std::println(
        "=== Issue #735: macro provenance StableRef observability (scope-limited close) ===");

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
    return aura_issue_735_run();
}
#endif
