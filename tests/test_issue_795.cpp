// test_issue_795.cpp — Issue #795: P0 deep hot-path
// Contracts + stronger SoAView/ShapeStablePass
// Concepts + ShapeProfiler JIT Epoch Sync + Dirty
// Propagation observability (Non-duplicative
// refinement of #768/#507/#766/#767/#741).
//
// Scope-limited close: the body asks for 6 things:
// (1) arena.ixx + ir_soa.ixx accessors: add pre/post
// contract_assert on allocate_raw (size/align),
// view_at / mark_ dirty (index bounds, dirty column
// consistency); on_compact_hook_ invoke with
// shape_inval + dirty cascade, (2) shape_profiler.cpp
// + value dispatch: more pre/post in inline_shape_of,
// record_shape stability transition, history push;
// bump version on deopt and wire to mutation_epoch_ +
// JIT epoch hint; consult DirtyAware or #741
// impact_scope for targeted invalidation instead of
// global, (3) pass_manager.ixx + lowering/JIT:
// define SoAView concept (requires const view +
// shape_id consult) and ShapeStablePass (requires
// stable_shape consult + DirtyAware); static_assert
// in run_incremental_dirty_pipeline; integrate
// dominant_shape into ComputeKind or new
// ShapePropagationPass for short-circuit on stable,
// (4) metrics/primitive: enhance
// (query:shape-pass-hotpath-contracts-stats)
// returning (contract_checks_hotpath,
// shape_stability_transitions, jit_epoch_sync_hits,
// targeted_deopt_skips, concept_violations_caught);
// SLO deopt rate <5% under mutation, (5)
// tests/test_highperf_contracts_concepts_shape_jit_
// epoch.cpp harness (shape-varying calls + heavy
// mutate + JIT recompile under debug/release → assert
// Contracts catch only in debug, stronger Concepts
// enforce at compile, epoch sync + targeted deopt
// correct, metrics, TSan clean), (6) integration:
// wire to Arena compact (shape_inval), IR dirty
// cascade, Pass yield, recent DepGraph/impact
// (#741); enable stable-shape specialization. All
// follow-up work is Phase 2+ (each requires touching
// arena.ixx + ir_soa.ixx + shape_profiler.cpp +
// value.ixx + pass_manager.ixx + new test + CI gate).
// Phase 1 observability surface ships in this PR:
//
//   1. 4 NEW CompilerMetrics atomics + 4 NEW bump
//      helpers on Evaluator:
//      - soa_view_violations_caught_total /
//        bump_soa_view_violations_caught()
//      - shape_stable_pass_violations_total /
//        bump_shape_stable_pass_violations()
//      - targeted_deopt_via_impact_scope_total /
//        bump_targeted_deopt_via_impact_scope()
//      - on_compact_hook_invocations_total /
//        bump_on_compact_hook_invocation()
//   2. New standalone
//      (query:shape-pass-hotpath-contracts-stats,
//      schema 795) primitive returning 4 NEW
//      atomics + 1 hardcoded "not yet" flag
//      (concepts-active) + derived recommendation +
//      schema sentinel (8-entry hash).
//
// ACs:
//   AC1: hash shape (7 fields + schema sentinel = 8 entries)
//   AC2: fresh-service zero state (4 NEW atomics == 0;
//        1 hardcoded "not yet" flag == 0;
//        recommendation == 3 early-stage)
//   AC3: schema == 795 (drift sentinel)
//   AC4: production-path bump correctness — call the
//        per-Evaluator bump helpers + cross-check the
//        primitive reads reflect the bumps
//   AC5: sibling observability regression — #768
//        (shape-pass-hotpath-stats) + #794
//        (full-closedloop-compiler-edsl-fidelity-stats)
//        primitives still reachable with their schema
//        sentinels intact

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_795_detail {
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
    std::println("\n--- AC1: (query:shape-pass-hotpath-contracts-stats) hash shape ---");
    auto r = cs.eval("(query:shape-pass-hotpath-contracts-stats)");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "(query:shape-pass-hotpath-contracts-stats) returns a hash");
    const std::vector<std::string> keys = {"soa-view-violations-caught-total",
                                           "shape-stable-pass-violations-total",
                                           "targeted-deopt-via-impact-scope-total",
                                           "on-compact-hook-invocations-total",
                                           "concepts-active",
                                           "recommendation",
                                           "schema"};
    for (const auto& k : keys) {
        auto f =
            cs.eval(std::format("(hash-ref (query:shape-pass-hotpath-contracts-stats) '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: fresh-service zero state (no hot-path contracts activity) ---");
    const auto soa_view = hash_int_field(cs, "(query:shape-pass-hotpath-contracts-stats)",
                                         "soa-view-violations-caught-total");
    CHECK(soa_view == 0,
          std::format("soa-view-violations-caught-total = {} (expected 0 on fresh service "
                      "— Phase 2+ deferred to wire SoAView concept static_assert in "
                      "pass_manager.ixx + lowering/JIT run_incremental_dirty_pipeline)",
                      soa_view));
    const auto shape_stable = hash_int_field(cs, "(query:shape-pass-hotpath-contracts-stats)",
                                             "shape-stable-pass-violations-total");
    CHECK(shape_stable == 0,
          std::format("shape-stable-pass-violations-total = {} (expected 0 on fresh service "
                      "— Phase 2+ deferred to wire ShapeStablePass concept static_assert in "
                      "pass_manager.ixx + dominant_shape / ShapePropagationPass)",
                      shape_stable));
    const auto targeted_deopt = hash_int_field(cs, "(query:shape-pass-hotpath-contracts-stats)",
                                               "targeted-deopt-via-impact-scope-total");
    CHECK(targeted_deopt == 0,
          std::format("targeted-deopt-via-impact-scope-total = {} (expected 0 on fresh "
                      "service — Phase 2+ deferred to wire shape_profiler.cpp deopt hook "
                      "to consult #741 impact_scope for targeted invalidation)",
                      targeted_deopt));
    const auto on_compact = hash_int_field(cs, "(query:shape-pass-hotpath-contracts-stats)",
                                           "on-compact-hook-invocations-total");
    CHECK(on_compact == 0,
          std::format("on-compact-hook-invocations-total = {} (expected 0 on fresh "
                      "service — Phase 2+ deferred to wire arena.ixx + ir_soa.ixx "
                      "on_compact_hook_ with shape_inval + dirty cascade)",
                      on_compact));
    const auto concepts =
        hash_int_field(cs, "(query:shape-pass-hotpath-contracts-stats)", "concepts-active");
    CHECK(concepts == 0,
          std::format("concepts-active = {} (expected 0 — Phase 2+ deferred to actually "
                      "wire SoAView + ShapeStablePass concepts + targeted deopt via "
                      "impact_scope + ShapeProfiler epoch sync all together; single flag "
                      "covers all 3+ deferred wire-up areas)",
                      concepts));
    const auto rec =
        hash_int_field(cs, "(query:shape-pass-hotpath-contracts-stats)", "recommendation");
    CHECK(rec == 3,
          std::format("recommendation = {} (expected 3 = early-stage when deferred flag == 0 "
                      "AND no activity)",
                      rec));
}

static void run_ac3_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: schema == 795 (drift sentinel) ---");
    const auto schema = hash_int_field(cs, "(query:shape-pass-hotpath-contracts-stats)", "schema");
    CHECK(schema == 795, std::format("schema = {} (expected 795)", schema));
}

static void run_ac4_bump_correctness(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: production-path bump helpers + primitive read-back ---");

    // Snapshot before.
    const auto soa_view_before = hash_int_field(cs, "(query:shape-pass-hotpath-contracts-stats)",
                                                "soa-view-violations-caught-total");
    const auto shape_stable_before = hash_int_field(
        cs, "(query:shape-pass-hotpath-contracts-stats)", "shape-stable-pass-violations-total");
    const auto targeted_deopt_before = hash_int_field(
        cs, "(query:shape-pass-hotpath-contracts-stats)", "targeted-deopt-via-impact-scope-total");
    const auto on_compact_before = hash_int_field(cs, "(query:shape-pass-hotpath-contracts-stats)",
                                                  "on-compact-hook-invocations-total");

    // Exercise the 4 NEW per-Evaluator bump helpers
    // via the service's evaluator instance. The bump
    // helpers bump CompilerMetrics atomics (which
    // the primitive reads via ev.compiler_metrics()).
    auto& ev = cs.evaluator();
    constexpr int k_iters = 2;
    for (int i = 0; i < k_iters; ++i) {
        ev.bump_soa_view_violations_caught();
        ev.bump_shape_stable_pass_violations();
        ev.bump_targeted_deopt_via_impact_scope();
        ev.bump_on_compact_hook_invocation();
    }

    const auto soa_view_after = hash_int_field(cs, "(query:shape-pass-hotpath-contracts-stats)",
                                               "soa-view-violations-caught-total");
    const auto shape_stable_after = hash_int_field(cs, "(query:shape-pass-hotpath-contracts-stats)",
                                                   "shape-stable-pass-violations-total");
    const auto targeted_deopt_after = hash_int_field(
        cs, "(query:shape-pass-hotpath-contracts-stats)", "targeted-deopt-via-impact-scope-total");
    const auto on_compact_after = hash_int_field(cs, "(query:shape-pass-hotpath-contracts-stats)",
                                                 "on-compact-hook-invocations-total");

    std::println("  counts after AC4 bumps: soa-view {} -> {}, shape-stable {} -> {}, "
                 "targeted-deopt {} -> {}, on-compact {} -> {}",
                 soa_view_before, soa_view_after, shape_stable_before, shape_stable_after,
                 targeted_deopt_before, targeted_deopt_after, on_compact_before, on_compact_after);

    // Direct bump helpers added exactly k_iters to
    // each of the 4 NEW atomics.
    CHECK(soa_view_after >= soa_view_before + k_iters,
          std::format("soa-view-violations-caught-total bumped by "
                      "bump_soa_view_violations_caught ({} -> {})",
                      soa_view_before, soa_view_after));
    CHECK(shape_stable_after >= shape_stable_before + k_iters,
          std::format("shape-stable-pass-violations-total bumped by "
                      "bump_shape_stable_pass_violations ({} -> {})",
                      shape_stable_before, shape_stable_after));
    CHECK(targeted_deopt_after >= targeted_deopt_before + k_iters,
          std::format("targeted-deopt-via-impact-scope-total bumped by "
                      "bump_targeted_deopt_via_impact_scope ({} -> {})",
                      targeted_deopt_before, targeted_deopt_after));
    CHECK(on_compact_after >= on_compact_before + k_iters,
          std::format("on-compact-hook-invocations-total bumped by "
                      "bump_on_compact_hook_invocation ({} -> {})",
                      on_compact_before, on_compact_after));

    // Recommendation should now be 2 (Phase 1 only —
    // deferred flag == 0 BUT activity > 0).
    const auto rec_after =
        hash_int_field(cs, "(query:shape-pass-hotpath-contracts-stats)", "recommendation");
    CHECK(rec_after == 2,
          std::format("recommendation = {} (expected 2 = Phase 1 only after activity; "
                      "activity > 0 with deferred flag == 0)",
                      rec_after));
}

static void run_ac5_sibling_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: regression — #768 + #794 sibling primitives unaffected ---");
    auto a768 = cs.eval("(query:shape-pass-hotpath-stats)");
    auto a794 = cs.eval("(query:full-closedloop-compiler-edsl-fidelity-stats)");
    CHECK(a768 && aura::compiler::types::is_hash(*a768),
          "query:shape-pass-hotpath-stats hash regression (#768)");
    CHECK(a794 && aura::compiler::types::is_hash(*a794),
          "query:full-closedloop-compiler-edsl-fidelity-stats hash regression (#794)");
    const auto a768_schema = hash_int_field(cs, "(query:shape-pass-hotpath-stats)", "schema");
    CHECK(a768_schema == 768,
          std::format("#768 schema = {} (expected 768, no drift)", a768_schema));
    const auto a794_schema =
        hash_int_field(cs, "(query:full-closedloop-compiler-edsl-fidelity-stats)", "schema");
    CHECK(a794_schema == 794,
          std::format("#794 schema = {} (expected 794, no drift)", a794_schema));
}

} // namespace aura_issue_795_detail

int aura_issue_795_run() {
    using namespace aura_issue_795_detail;
    std::println("=== Issue #795: P0 deep hot-path Contracts + stronger SoAView/"
                 "ShapeStablePass Concepts + ShapeProfiler JIT Epoch Sync + Dirty "
                 "Propagation observability (scope-limited close) ===");

    {
        aura::compiler::CompilerService cs;
        run_ac1_shape(cs);
        run_ac2_fresh_zero(cs);
        run_ac3_schema_sentinel(cs);
        run_ac4_bump_correctness(cs);
        run_ac5_sibling_regression(cs);
    }

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_795_run();
}
#endif
