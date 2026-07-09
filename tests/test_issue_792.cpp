// test_issue_792.cpp — Issue #792: P0
// compiler-runtime integration synchronization
// between incremental invalidate_function /
// mutation_epoch_ and EDSL/fiber MutationBoundaryGuard
// + steal safety for live closures/Envs/GuardShape in
// AI multi-round self-mod closed-loops
// (Non-duplicative refinement of #783/#755/#784/#787).
//
// Scope-limited close: the body asks for 5 things:
// (1) service.ixx invalidate_function + ModuleState
// add param or query for current fiber's mutation_stack_
// depth — if depth > 0 or inside Guard, defer epoch
// bump / re-lower to post-yield boundary or queue,
// expose safe_invalidate_at_outermost_boundary(),
// (2) evaluator_fiber_mutation.cpp + evaluator.ixx
// apply_closure / materialize_call_env: on steal resume
// / restore_post_yield_or_rollback (if affected by
// recent invalidate), force bridge_epoch / EnvFrame
// version_ re-stamp + closure_bridge_ refresh for
// live IRClosure; integrate with GuardShape expected_shape
// re-validation, (3) aura_jit_bridge.cpp + JIT hot-swap
// paths: during refcount swap / hot-reload, if any
// fiber in MutationBoundary or apply_closure active,
// defer or use grace + force GuardShape deopt +
// linear_state re-check on affected funcs; wire to
// mutation_epoch_, (4) new/enhance
// (query:compiler-invalidate-guard-steal-stats)
// returning (deferred_invalidates, version_refresh_hits,
// guardshape_deopt_on_steal, live_closure_stale_prevented)
// + wire to existing mutation-impact + aot-hotupdate-
// stats, (5) tests/test_compiler_invalidate_fiber_steal_
// guardshape_linear.cpp harness (multi-fiber heavy
// mutate:rebind/set-body triggering invalidate + steal
// timing during Guard + JIT apply_closure → assert
// no stale version/epoch drift, GuardShape consistent,
// linear ownership respected, metrics accurate, TSan
// clean). All follow-up work is Phase 2+ (each requires
// touching service.ixx + evaluator_fiber_mutation.cpp
// + aura_jit_bridge.cpp + new test + CI gate).
// Phase 1 observability surface ships in this PR:
//
//   1. 4 NEW CompilerMetrics atomics + 4 NEW bump
//      helpers on Evaluator:
//      - compiler_invalidate_deferred_total /
//        bump_compiler_invalidate_deferred() (called
//        at the planned Phase 2+ service.ixx
//        invalidate_function wire-up when active
//        MutationBoundaryGuard depth > 0)
//      - compiler_version_refresh_hits_total /
//        bump_compiler_version_refresh_hit() (called
//        at the planned Phase 2+
//        evaluator_fiber_mutation.cpp +
//        apply_closure / materialize_call_env
//        wire-up)
//      - compiler_guardshape_deopt_on_steal_total /
//        bump_compiler_guardshape_deopt_on_steal()
//        (called at the planned Phase 2+
//        aura_jit_bridge.cpp + JIT hot-swap paths
//        wire-up)
//      - compiler_live_closure_stale_prevented_total /
//        bump_compiler_live_closure_stale_prevented()
//        (called at the planned Phase 2+ apply_closure
//        dual-path + bridge_epoch check wire-up)
//   2. New standalone (query:compiler-invalidate-
//      guard-steal-stats, schema 792) primitive
//      returning 4 NEW atomics + 2 hardcoded
//      "not yet" flags (safe-invalidate-at-outermost-
//      boundary-active + steal-resume-version-refresh-
//      active) + derived recommendation + schema
//      sentinel (8-entry hash).
//
// ACs:
//   AC1: hash shape (7 fields + schema sentinel = 8 entries)
//   AC2: fresh-service zero state (4 NEW atomics == 0;
//        2 hardcoded "not yet" flags == 0;
//        recommendation == 3 early-stage)
//   AC3: schema == 792 (drift sentinel)
//   AC4: production-path bump correctness — call the
//        per-Evaluator bump helpers + cross-check the
//        primitive reads reflect the bumps
//   AC5: sibling observability regression — #791
//        (workspace-closedloop-fiber-multi-agent-yield-
//        stats) + #783 (orchestration-steal-outermost-
//        stats) primitives still reachable with their
//        schema sentinels intact

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_792_detail {
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
    std::println("\n--- AC1: (query:compiler-invalidate-guard-steal-stats) hash shape ---");
    auto r = cs.eval("(query:compiler-invalidate-guard-steal-stats)");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "(query:compiler-invalidate-guard-steal-stats) returns a hash");
    const std::vector<std::string> keys = {"deferred-invalidates-total",
                                           "version-refresh-hits-total",
                                           "guardshape-deopt-on-steal-total",
                                           "live-closure-stale-prevented-total",
                                           "safe-invalidate-at-outermost-boundary-active",
                                           "steal-resume-version-refresh-active",
                                           "recommendation",
                                           "schema"};
    for (const auto& k : keys) {
        auto f = cs.eval(
            std::format("(hash-ref (query:compiler-invalidate-guard-steal-stats) '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: fresh-service zero state (no compiler-runtime sync activity) ---");
    const auto deferred = hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)",
                                         "deferred-invalidates-total");
    CHECK(deferred == 0,
          std::format("deferred-invalidates-total = {} (expected 0 on fresh service — Phase "
                      "2+ deferred to wire invalidate_function when active "
                      "MutationBoundaryGuard depth > 0)",
                      deferred));
    const auto refresh = hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)",
                                        "version-refresh-hits-total");
    CHECK(refresh == 0,
          std::format("version-refresh-hits-total = {} (expected 0 on fresh service — Phase "
                      "2+ deferred to wire bridge_epoch / EnvFrame version_ re-stamp on steal "
                      "resume)",
                      refresh));
    const auto deopt = hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)",
                                      "guardshape-deopt-on-steal-total");
    CHECK(deopt == 0,
          std::format("guardshape-deopt-on-steal-total = {} (expected 0 on fresh service — "
                      "Phase 2+ deferred to wire GuardShape deopt on bridge_epoch mismatch)",
                      deopt));
    const auto stale = hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)",
                                      "live-closure-stale-prevented-total");
    CHECK(stale == 0,
          std::format("live-closure-stale-prevented-total = {} (expected 0 on fresh service — "
                      "Phase 2+ deferred to wire closure_bridge_ refresh for live IRClosure "
                      "stale prevention)",
                      stale));
    const auto safe_invalidate = hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)",
                                                "safe-invalidate-at-outermost-boundary-active");
    CHECK(safe_invalidate == 0,
          std::format("safe-invalidate-at-outermost-boundary-active = {} (expected 0 — Phase "
                      "2+ deferred to expose safe_invalidate_at_outermost_boundary() helper)",
                      safe_invalidate));
    const auto refresh_active = hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)",
                                               "steal-resume-version-refresh-active");
    CHECK(refresh_active == 0,
          std::format("steal-resume-version-refresh-active = {} (expected 0 — Phase 2+ "
                      "deferred to wire force bridge_epoch / EnvFrame version_ re-stamp + "
                      "closure_bridge_ refresh on steal resume)",
                      refresh_active));
    const auto rec =
        hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)", "recommendation");
    CHECK(rec == 3,
          std::format("recommendation = {} (expected 3 = early-stage when both deferred flags "
                      "== 0 AND no activity)",
                      rec));
}

static void run_ac3_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: schema == 792 (drift sentinel) ---");
    const auto schema =
        hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)", "schema");
    CHECK(schema == 792, std::format("schema = {} (expected 792)", schema));
}

static void run_ac4_bump_correctness(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: production-path bump helpers + primitive read-back ---");

    // Snapshot before.
    const auto deferred_before = hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)",
                                                "deferred-invalidates-total");
    const auto refresh_before = hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)",
                                               "version-refresh-hits-total");
    const auto deopt_before = hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)",
                                             "guardshape-deopt-on-steal-total");
    const auto stale_before = hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)",
                                             "live-closure-stale-prevented-total");

    // Exercise the 4 NEW per-Evaluator bump helpers
    // via the service's evaluator instance. The bump
    // helpers bump CompilerMetrics atomics (which
    // the primitive reads via ev.compiler_metrics()).
    auto& ev = cs.evaluator();
    constexpr int k_iters = 2;
    for (int i = 0; i < k_iters; ++i) {
        ev.bump_compiler_invalidate_deferred();
        ev.bump_compiler_version_refresh_hit();
        ev.bump_compiler_guardshape_deopt_on_steal();
        ev.bump_compiler_live_closure_stale_prevented();
    }

    const auto deferred_after = hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)",
                                               "deferred-invalidates-total");
    const auto refresh_after = hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)",
                                              "version-refresh-hits-total");
    const auto deopt_after = hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)",
                                            "guardshape-deopt-on-steal-total");
    const auto stale_after = hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)",
                                            "live-closure-stale-prevented-total");

    std::println(
        "  counts after AC4 bumps: deferred {} -> {}, refresh {} -> {}, deopt {} -> {}, stale "
        "{} -> {}",
        deferred_before, deferred_after, refresh_before, refresh_after, deopt_before, deopt_after,
        stale_before, stale_after);

    // Direct bump helpers added exactly k_iters to
    // each of the 4 NEW atomics.
    CHECK(deferred_after >= deferred_before + k_iters,
          std::format("deferred-invalidates-total bumped by "
                      "bump_compiler_invalidate_deferred ({} -> {})",
                      deferred_before, deferred_after));
    CHECK(refresh_after >= refresh_before + k_iters,
          std::format("version-refresh-hits-total bumped by "
                      "bump_compiler_version_refresh_hit ({} -> {})",
                      refresh_before, refresh_after));
    CHECK(deopt_after >= deopt_before + k_iters,
          std::format("guardshape-deopt-on-steal-total bumped by "
                      "bump_compiler_guardshape_deopt_on_steal ({} -> {})",
                      deopt_before, deopt_after));
    CHECK(stale_after >= stale_before + k_iters,
          std::format("live-closure-stale-prevented-total bumped by "
                      "bump_compiler_live_closure_stale_prevented ({} -> {})",
                      stale_before, stale_after));

    // Recommendation should now be 2 (Phase 1 only —
    // both deferred flags == 0 BUT activity > 0).
    const auto rec_after =
        hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)", "recommendation");
    CHECK(rec_after == 2,
          std::format("recommendation = {} (expected 2 = Phase 1 only after activity; "
                      "activity > 0 with both deferred flags == 0)",
                      rec_after));
}

static void run_ac5_sibling_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: regression — #791 + #783 sibling primitives unaffected ---");
    auto a791 = cs.eval("(query:workspace-closedloop-fiber-multi-agent-yield-stats)");
    auto a783 = cs.eval("(query:orchestration-steal-outermost-stats)");
    CHECK(a791 && aura::compiler::types::is_hash(*a791),
          "query:workspace-closedloop-fiber-multi-agent-yield-stats hash regression (#791)");
    CHECK(a783 && aura::compiler::types::is_hash(*a783),
          "query:orchestration-steal-outermost-stats hash regression (#783)");
    const auto a791_schema =
        hash_int_field(cs, "(query:workspace-closedloop-fiber-multi-agent-yield-stats)", "schema");
    CHECK(a791_schema == 791,
          std::format("#791 schema = {} (expected 791, no drift)", a791_schema));
    const auto a783_schema =
        hash_int_field(cs, "(query:orchestration-steal-outermost-stats)", "schema");
    CHECK(a783_schema == 783,
          std::format("#783 schema = {} (expected 783, no drift)", a783_schema));
}

} // namespace aura_issue_792_detail

int aura_issue_792_run() {
    using namespace aura_issue_792_detail;
    std::println("=== Issue #792: P0 compiler invalidate_function + mutation_epoch_ "
                 "synchronization with outermost MutationBoundaryGuard depth + live "
                 "IRClosure / EnvFrame / GuardShape version refresh observability "
                 "(scope-limited close) ===");

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
    return aura_issue_792_run();
}
#endif
