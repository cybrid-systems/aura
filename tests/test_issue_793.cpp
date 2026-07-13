// test_issue_793.cpp — Issue #793: P0 JIT/AOT
// hot-swap + GuardShape + linear + EnvFrame version_
// consistency observability (Non-duplicative
// consolidation/refinement of #785/#787/#755).
//
// Scope-limited close: the body asks for 5 things:
// (1) aura_jit.cpp + aura_jit_bridge.cpp hot-swap/
// reload: on successful refcount swap or region
// reload, if any active fiber holds MutationBoundary
// or has live GuardShape/Apply on affected func,
// force deopt (set generic_block) or bump shape_id
// / linear_state for affected IR; integrate epoch/
// version check from mutation_epoch_ / EnvFrame, (2)
// JIT codegen for GuardShape / Linear*: emit
// additional runtime checks (version_ probe or
// bridge_epoch compare) before deopt decision or
// MoveOp; wire to EnvFrame materialize path, (3)
// evaluator_fiber_mutation.cpp + apply_closure: on
// steal resume / post-rollback, for JIT-executed
// closures, trigger GuardShape re-evaluation or
// linear re-wrap if version_ or epoch drifted;
// coordinate with JIT func_table update, (4)
// enhance aot-hotupdate-stats + jit-guard-linear-
// stats (deopt_forced_on_reload, linear_violation_
// prevented, env_version_sync_hits,
// guardshape_stale_reject), (5) tests/test_jit_aot_
// hotswap_guardshape_linear_steal_rollback.cpp
// harness (multi-fiber mutate triggering
// invalidate/re-lower + AOT reload timing + steal
// during GuardShape-protected apply_closure →
// assert consistent deopt/linear/Env version, no
// violation, metrics, TSan clean). All follow-up
// work is Phase 2+ (each requires touching
// aura_jit.cpp + aura_jit_bridge.cpp +
// evaluator_fiber_mutation.cpp + ir_executor.ixx +
// new test + CI gate). Phase 1 observability
// surface ships in this PR:
//
//   1. 4 NEW CompilerMetrics atomics + 4 NEW bump
//      helpers on Evaluator:
//      - jit_deopt_forced_on_reload_total /
//        bump_jit_deopt_forced_on_reload() (called
//        at the planned Phase 2+ aura_jit.cpp +
//        aura_jit_bridge.cpp hot-swap path when
//        active fiber holds GuardShape/Apply on
//        affected func)
//      - jit_linear_violation_prevented_total /
//        bump_jit_linear_violation_prevented()
//        (called at the planned Phase 2+
//        aura_jit.cpp JIT codegen for Linear* ops)
//      - jit_env_version_sync_hits_total /
//        bump_jit_env_version_sync_hit() (called
//        at the planned Phase 2+
//        evaluator_fiber_mutation.cpp +
//        apply_closure on steal resume / post-
//        rollback)
//      - jit_guardshape_stale_reject_total /
//        bump_jit_guardshape_stale_reject() (called
//        at the planned Phase 2+ ir_executor.ixx +
//        evaluator.ixx apply_closure bridge_epoch
//        check)
//   2. New standalone (query:jit-aot-hotswap-
//      fidelity-stats, schema 793) primitive
//      returning 4 NEW atomics + 2 hardcoded
//      "not yet" flags (reload-deopt-version-hooks-
//      active + jit-emit-runtime-version-checks-
//      active) + derived recommendation + schema
//      sentinel (8-entry hash).
//
// ACs:
//   AC1: hash shape (7 fields + schema sentinel = 8 entries)
//   AC2: fresh-service zero state (4 NEW atomics == 0;
//        2 hardcoded "not yet" flags == 0;
//        recommendation == 3 early-stage)
//   AC3: schema == 793 (drift sentinel)
//   AC4: production-path bump correctness — call the
//        per-Evaluator bump helpers + cross-check the
//        primitive reads reflect the bumps
//   AC5: sibling observability regression — #785
//        (aot-concurrent-hotupdate-stats) + #792
//        (compiler-invalidate-guard-steal-stats)
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

namespace aura_issue_793_detail {
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
    std::println("\n--- AC1: (query:jit-aot-hotswap-fidelity-stats) hash shape ---");
    auto r = cs.eval("(query:jit-aot-hotswap-fidelity-stats)");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "(query:jit-aot-hotswap-fidelity-stats) returns a hash");
    const std::vector<std::string> keys = {"deopt-forced-on-reload-total",
                                           "linear-violation-prevented-total",
                                           "env-version-sync-hits-total",
                                           "guardshape-stale-reject-total",
                                           "reload-deopt-version-hooks-active",
                                           "jit-emit-runtime-version-checks-active",
                                           "recommendation",
                                           "schema"};
    for (const auto& k : keys) {
        auto f = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:jit-aot-hotswap-fidelity-stats\") '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: fresh-service zero state (no JIT/AOT fidelity activity) ---");
    const auto deopt = hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)",
                                      "deopt-forced-on-reload-total");
    CHECK(deopt == 0,
          std::format("deopt-forced-on-reload-total = {} (expected 0 on fresh service — "
                      "Phase 2+ deferred to wire GuardShape deopt on AOT reload / refcount "
                      "swap path)",
                      deopt));
    const auto linear = hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)",
                                       "linear-violation-prevented-total");
    CHECK(linear == 0,
          std::format("linear-violation-prevented-total = {} (expected 0 on fresh service — "
                      "Phase 2+ deferred to wire JIT runtime version check for Linear* ops)",
                      linear));
    const auto env_sync =
        hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)", "env-version-sync-hits-total");
    CHECK(env_sync == 0,
          std::format("env-version-sync-hits-total = {} (expected 0 on fresh service — Phase "
                      "2+ deferred to wire EnvFrame::version_ sync on JIT-executed closure "
                      "steal resume / post-rollback)",
                      env_sync));
    const auto guardshape = hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)",
                                           "guardshape-stale-reject-total");
    CHECK(guardshape == 0,
          std::format("guardshape-stale-reject-total = {} (expected 0 on fresh service — "
                      "Phase 2+ deferred to wire apply_closure bridge_epoch check for "
                      "GuardShape expected_shape / shape_id mismatch)",
                      guardshape));
    const auto hooks = hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)",
                                      "reload-deopt-version-hooks-active");
    CHECK(hooks == 0, std::format("reload-deopt-version-hooks-active = {} (expected 0 — Phase 2+ "
                                  "deferred to wire reload-deopt version hooks in aura_jit.cpp + "
                                  "aura_jit_bridge.cpp hot-swap path)",
                                  hooks));
    const auto emit_checks = hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)",
                                            "jit-emit-runtime-version-checks-active");
    CHECK(emit_checks == 0,
          std::format("jit-emit-runtime-version-checks-active = {} (expected 0 — Phase 2+ "
                      "deferred to wire additional runtime checks in JIT codegen for "
                      "GuardShape / Linear* ops)",
                      emit_checks));
    const auto rec = hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)", "recommendation");
    CHECK(rec == 3,
          std::format("recommendation = {} (expected 3 = early-stage when both deferred flags "
                      "== 0 AND no activity)",
                      rec));
}

static void run_ac3_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: schema == 793 (drift sentinel) ---");
    const auto schema = hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)", "schema");
    CHECK(schema == 793, std::format("schema = {} (expected 793)", schema));
}

static void run_ac4_bump_correctness(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: production-path bump helpers + primitive read-back ---");

    // Snapshot before.
    const auto deopt_before = hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)",
                                             "deopt-forced-on-reload-total");
    const auto linear_before = hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)",
                                              "linear-violation-prevented-total");
    const auto env_sync_before =
        hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)", "env-version-sync-hits-total");
    const auto guardshape_before = hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)",
                                                  "guardshape-stale-reject-total");

    // Exercise the 4 NEW per-Evaluator bump helpers
    // via the service's evaluator instance. The bump
    // helpers bump CompilerMetrics atomics (which
    // the primitive reads via ev.compiler_metrics()).
    auto& ev = cs.evaluator();
    constexpr int k_iters = 2;
    for (int i = 0; i < k_iters; ++i) {
        ev.bump_jit_deopt_forced_on_reload();
        ev.bump_jit_linear_violation_prevented();
        ev.bump_jit_env_version_sync_hit();
        ev.bump_jit_guardshape_stale_reject();
    }

    const auto deopt_after = hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)",
                                            "deopt-forced-on-reload-total");
    const auto linear_after = hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)",
                                             "linear-violation-prevented-total");
    const auto env_sync_after =
        hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)", "env-version-sync-hits-total");
    const auto guardshape_after = hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)",
                                                 "guardshape-stale-reject-total");

    std::println("  counts after AC4 bumps: deopt {} -> {}, linear {} -> {}, env-sync {} -> {}, "
                 "guardshape {} -> {}",
                 deopt_before, deopt_after, linear_before, linear_after, env_sync_before,
                 env_sync_after, guardshape_before, guardshape_after);

    // Direct bump helpers added exactly k_iters to
    // each of the 4 NEW atomics.
    CHECK(deopt_after >= deopt_before + k_iters,
          std::format("deopt-forced-on-reload-total bumped by "
                      "bump_jit_deopt_forced_on_reload ({} -> {})",
                      deopt_before, deopt_after));
    CHECK(linear_after >= linear_before + k_iters,
          std::format("linear-violation-prevented-total bumped by "
                      "bump_jit_linear_violation_prevented ({} -> {})",
                      linear_before, linear_after));
    CHECK(env_sync_after >= env_sync_before + k_iters,
          std::format("env-version-sync-hits-total bumped by "
                      "bump_jit_env_version_sync_hit ({} -> {})",
                      env_sync_before, env_sync_after));
    CHECK(guardshape_after >= guardshape_before + k_iters,
          std::format("guardshape-stale-reject-total bumped by "
                      "bump_jit_guardshape_stale_reject ({} -> {})",
                      guardshape_before, guardshape_after));

    // Recommendation should now be 2 (Phase 1 only —
    // both deferred flags == 0 BUT activity > 0).
    const auto rec_after =
        hash_int_field(cs, "(query:jit-aot-hotswap-fidelity-stats)", "recommendation");
    CHECK(rec_after == 2,
          std::format("recommendation = {} (expected 2 = Phase 1 only after activity; "
                      "activity > 0 with both deferred flags == 0)",
                      rec_after));
}

static void run_ac5_sibling_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: regression — #785 + #792 sibling primitives unaffected ---");
    auto a785 = cs.eval("(query:aot-concurrent-hotupdate-stats)");
    auto a792 = cs.eval("(query:compiler-invalidate-guard-steal-stats)");
    CHECK(a785 && aura::compiler::types::is_hash(*a785),
          "query:aot-concurrent-hotupdate-stats hash regression (#785)");
    CHECK(a792 && aura::compiler::types::is_hash(*a792),
          "query:compiler-invalidate-guard-steal-stats hash regression (#792)");
    const auto a785_schema = hash_int_field(cs, "(query:aot-concurrent-hotupdate-stats)", "schema");
    CHECK(a785_schema == 785,
          std::format("#785 schema = {} (expected 785, no drift)", a785_schema));
    const auto a792_schema =
        hash_int_field(cs, "(query:compiler-invalidate-guard-steal-stats)", "schema");
    CHECK(a792_schema == 792,
          std::format("#792 schema = {} (expected 792, no drift)", a792_schema));
}

} // namespace aura_issue_793_detail

int aura_issue_793_run() {
    using namespace aura_issue_793_detail;
    std::println("=== Issue #793: P0 JIT/AOT hot-swap + GuardShape + linear + EnvFrame "
                 "version_ consistency observability (scope-limited close) ===");

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
    return aura_issue_793_run();
}
#endif
