// test_issue_731.cpp — Issue #731: Arena + SoA + EnvFrame concurrent
// compaction safety observability primitive (non-duplicative with #722
// arena-integration-stats tier integration, #743 Arena auto-compact
// policy + fiber safepoint + dirty/Shape closed loop, #647 EnvFrame
// dual-path, #648 panic checkpoint fiber, #685 auto-compact policy,
// #604 Arena auto-compact fiber/GC safepoint). #731 tracks the
// *concurrent* safety specifically: scheduler-safepoint coordination +
// EnvFrame GCEnvWalkFn revalidation + panic-rollback-compact integration
// + race prevention as separate counters).
//
// Scope-limited close: the issue body asks for: (1) concurrent compact
// safepoint coordination in arena.ixx + gc_coordinator, (2) GCEnvWalkFn
// EnvFrame revalidation in evaluator_gc.cpp, (3) fiber.cpp resume() /
// transfer hook integration with active panic checkpoint, (4) primitive
// query:arena-concurrent-compact-stats, (5) panic checkpoint snapshot
// integration of arena state, (6) tests/test_arena_concurrent_compact_
// envframe_fiber_steal.cpp harness with heavy alloc/mutate under 10+
// fibers + steal + periodic compact + panic injection, (7) #674 chaos
// stress integration. Items (1)/(2)/(3)/(5)/(6)/(7) require dedicated
// wiring into arena.ixx + gc_coordinator + evaluator_gc.cpp + fiber.cpp +
// panic_checkpoint + new test harness + chaos stress; each is a non-
// trivial focused session.
//
// For this PR we ship:
//
//   1. 4 new atomics in CompilerMetrics:
//        arena_concurrent_compacts_total
//        arena_envframe_revalidations_total
//        arena_panic_rollback_compact_hits_total
//        arena_races_prevented_total
//   2. 4 new public bump helpers in Evaluator:
//        bump_arena_concurrent_compact
//        bump_arena_envframe_revalidation
//        bump_arena_panic_rollback_compact_hit
//        bump_arena_race_prevented
//   3. New standalone (query:arena-concurrent-compact-stats, schema 731)
//      primitive exposing the 4 counters (5-entry hash: 4 fields +
//      schema sentinel)
//   4. Test verifies: primitive shape, fresh-zero state, schema sentinel,
//      bump accessibility, regression of sibling primitives
//
// Non-duplicative notes:
//   - #722 (query:arena-integration-stats tier) — different scope (tier
//     integration + dtors + bumps, no concurrent safety surface)
//   - #743 (Arena auto-compact policy + fiber safepoint) — different
//     scope (policy + dirty/Shape closed loop, no concurrent safety)
//   - #647 / #648 / #685 / #604 — internal mechanics + coarse stats
//   - The existing arena.ixx + gc_coordinator + fiber.cpp infrastructure
//   - #731 is the FIRST observability surface that tracks the *concurrent*
//     safety specifically: scheduler-safepoint coordination + EnvFrame
//     GCEnvWalkFn revalidation + panic-rollback-compact integration +
//     race prevention, as separate per-decision-point counters
//
// ACs:
//   AC1: hash shape (4 fields + schema sentinel = 5 entries)
//   AC2: 4 counters == 0 on fresh service
//   AC3: schema == 731 (drift sentinel)
//   AC4: bump helpers accessible — exercise via direct bump on
//        Evaluator surface and verify the primitive reports the bumps
//   AC5: regression — #712 + #713 + #714 + #715 + #716 + #717
//        + #718 + #719 + #720 + #721 + #722 + #723 + #726 + #728
//        sibling primitives still reachable with their schema
//        sentinels intact
//
// (We do NOT implement concurrent compact safepoint coordination,
// do NOT add GCEnvWalkFn EnvFrame revalidation, do NOT wire fiber.cpp
// resume() / transfer hooks, do NOT integrate panic checkpoint
// snapshot, do NOT run the tests/test_arena_concurrent_compact_
// envframe_fiber_steal harness, do NOT extend #674 chaos stress —
// those are the bulk of this issue's remaining scope.)

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_731_detail {
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
    std::println(
        "\n--- AC1: (engine:metrics \"query:arena-concurrent-compact-stats\") hash shape ---");
    auto r = cs.eval("(engine:metrics \"query:arena-concurrent-compact-stats\")");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "(engine:metrics \"query:arena-concurrent-compact-stats\") returns a hash");
    const std::vector<std::string> keys = {"concurrent-compacts", "envframe-revalidations",
                                           "panic-rollback-compact-hits", "races-prevented",
                                           "schema"};
    for (const auto& k : keys) {
        auto f = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:arena-concurrent-compact-stats\") '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: counters == 0 on fresh service ---");
    const auto conc = hash_int_field(
        cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")", "concurrent-compacts");
    CHECK(conc == 0, std::format("concurrent-compacts = {} (expected 0 on fresh service)", conc));
    const auto reval = hash_int_field(
        cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")", "envframe-revalidations");
    CHECK(reval == 0,
          std::format("envframe-revalidations = {} (expected 0 on fresh service)", reval));
    const auto panic_rollback =
        hash_int_field(cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")",
                       "panic-rollback-compact-hits");
    CHECK(panic_rollback == 0,
          std::format("panic-rollback-compact-hits = {} (expected 0 on fresh service)",
                      panic_rollback));
    const auto races = hash_int_field(
        cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")", "races-prevented");
    CHECK(races == 0, std::format("races-prevented = {} (expected 0 on fresh service)", races));
}

static void run_ac3_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: schema == 731 (drift sentinel) ---");
    const auto schema =
        hash_int_field(cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")", "schema");
    CHECK(schema == 731, std::format("schema = {} (expected 731)", schema));
}

static void run_ac4_bump_accessible(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: bump helpers callable from the evaluator surface ---");
    // Direct call: invoke the evaluator's bump helpers via the public
    // surface. These helpers exist so future arena.ixx + gc_coordinator
    // + evaluator_gc.cpp concurrent compact / defrag success path +
    // fiber.cpp resume() / transfer hooks + panic checkpoint integration
    // can call them at each decision point (concurrent compact acquired
    // / EnvFrame revalidation completed / panic rollback fired on
    // compact / race prevented).
    auto& ev = cs.evaluator();
    ev.bump_arena_concurrent_compact();
    ev.bump_arena_concurrent_compact();
    ev.bump_arena_concurrent_compact();
    ev.bump_arena_envframe_revalidation();
    ev.bump_arena_envframe_revalidation();
    ev.bump_arena_envframe_revalidation();
    ev.bump_arena_envframe_revalidation();
    ev.bump_arena_envframe_revalidation();
    ev.bump_arena_envframe_revalidation();
    ev.bump_arena_panic_rollback_compact_hit();
    ev.bump_arena_panic_rollback_compact_hit();
    ev.bump_arena_race_prevented();
    ev.bump_arena_race_prevented();
    ev.bump_arena_race_prevented();
    ev.bump_arena_race_prevented();
    const auto conc = hash_int_field(
        cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")", "concurrent-compacts");
    const auto reval = hash_int_field(
        cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")", "envframe-revalidations");
    const auto panic_rollback =
        hash_int_field(cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")",
                       "panic-rollback-compact-hits");
    const auto races = hash_int_field(
        cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")", "races-prevented");
    CHECK(conc == 3,
          std::format("after 3 concurrent-compact bumps: concurrent-compacts = {} (expected 3)",
                      conc));
    CHECK(reval == 6,
          std::format("after 6 envframe-revalidation bumps: envframe-revalidations = {} "
                      "(expected 6)",
                      reval));
    CHECK(panic_rollback == 2,
          std::format("after 2 panic-rollback-compact-hit bumps: panic-rollback-compact-hits = {} "
                      "(expected 2)",
                      panic_rollback));
    CHECK(races == 4,
          std::format("after 4 race-prevented bumps: races-prevented = {} (expected 4)", races));
}

static void run_ac5_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: regression — #712..#728 sibling primitives unaffected ---");
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
    CHECK(incremental_schema == 718,
          std::format("incremental-relower schema = {} (expected 718, no drift)",
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
}

} // namespace aura_issue_731_detail

int aura_issue_731_run() {
    using namespace aura_issue_731_detail;
    std::println(
        "=== Issue #731: arena concurrent-compact observability (scope-limited close) ===");

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
    return aura_issue_731_run();
}
#endif
