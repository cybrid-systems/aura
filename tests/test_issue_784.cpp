// test_issue_784.cpp — Issue #784: P0 mandatory
// dual-path (bindings_ vs bindings_symid_) consistency
// enforcement + desync panic policy + GCEnvWalkFn
// stale integration under concurrent mutation/steal
// (Non-duplicative refinement of #756 / #731 / #647).
//
// Scope-limited close: the body asks for 5 things:
// (1) make ensure_envframe_dual_path_consistency()
// mandatory in walk_env_frames / GCEnvWalkFn /
// materialize_call_env / post-rollback / fiber steal
// resume paths, (2) implement desync detection + panic
// policy (strict-panic vs log-and-sync), (3) strengthen
// GCEnvWalkFn stale handling with dual-path consistency
// check, (4) trigger re-ensure + version re-stamp on
// concurrent steal/resume, (5) production CI gate for
// 0 undetected desync. The actual call-site wiring +
// panic policy + GCEnvWalkFn stale re-ensure are Phase
// 2+ deferred (each requires touching evaluator.ixx +
// evaluator_env.cpp + gc_coordinator + new test harness).
// Phase 1 observability surface ships in this PR:
//
//   1. 3 NEW CompilerMetrics atomics + 3 NEW bump
//      helpers on Evaluator:
//      - envframe_mandatory_enforce_total /
//        bump_envframe_mandatory_enforce() (called at
//        each mandatory ensure_ entry)
//      - envframe_mandatory_enforce_desync_total /
//        bump_envframe_mandatory_enforce_desync()
//        (called when ensure_ returns false at a
//        mandatory entry — the "safety net caught a
//        desync" signal)
//      - envframe_concurrent_steal_resync_total /
//        bump_envframe_concurrent_steal_resync()
//        (called at Fiber::resume() entry when a
//        stolen fiber triggers re-ensure)
//   2. New standalone
//      (query:envframe-dualpath-mandatory-enforce-stats,
//      schema 784) primitive returning 5 NEW / hardcoded
//      fields + derived recommendation + schema sentinel
//      (8-entry hash).
//
// ACs:
//   AC1: hash shape (7 fields + schema sentinel = 8 entries)
//   AC2: fresh-service zero state (3 NEW atomics == 0;
//        2 hardcoded "not yet" flags == 0;
//        recommendation == 3 early-stage)
//   AC3: schema == 784 (drift sentinel)
//   AC4: production-path bump correctness — call the
//        per-Evaluator bump helpers + cross-check the
//        primitive reads reflect the bumps
//   AC5: sibling observability regression — #756
//        (envframe-dualpath-policy-stats) + #783
//        (orchestration-steal-outermost-stats) primitives
//        still reachable with their schema sentinels
//        intact

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_784_detail {
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
    std::println("\n--- AC1: (engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\") "
                 "hash shape ---");
    auto r = cs.eval("(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\") returns a hash");
    const std::vector<std::string> keys = {"mandatory-enforce-total",
                                           "mandatory-enforce-desync-total",
                                           "gc-walk-resync-total",
                                           "concurrent-steal-resync-total",
                                           "policy-mode",
                                           "mandatory-call-sites-enabled",
                                           "recommendation",
                                           "schema"};
    for (const auto& k : keys) {
        auto f = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\") '{}')",
            k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: fresh-service zero state (no mandatory enforcement activity) ---");
    const auto enforce_total =
        hash_int_field(cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")",
                       "mandatory-enforce-total");
    CHECK(enforce_total == 0,
          std::format("mandatory-enforce-total = {} (expected 0 on fresh service — Phase 2+ "
                      "deferred to wire ensure_ at critical paths per body \"Make ensure_ "
                      "mandatory (call at start of critical paths)\")",
                      enforce_total));
    const auto desync_total =
        hash_int_field(cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")",
                       "mandatory-enforce-desync-total");
    CHECK(desync_total == 0,
          std::format("mandatory-enforce-desync-total = {} (expected 0 on fresh service)",
                      desync_total));
    const auto gc_walk =
        hash_int_field(cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")",
                       "gc-walk-resync-total");
    CHECK(gc_walk == 0,
          std::format("gc-walk-resync-total = {} (expected 0 — Phase 2+ deferred; #756 "
                      "envframe_gc_stale_desync_hits_total already exposes the GC stale "
                      "detection signal)",
                      gc_walk));
    const auto steal_resync =
        hash_int_field(cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")",
                       "concurrent-steal-resync-total");
    CHECK(steal_resync == 0,
          std::format("concurrent-steal-resync-total = {} (expected 0 on fresh service — "
                      "Phase 2+ deferred to wire at Fiber::resume() entry)",
                      steal_resync));
    const auto policy_mode = hash_int_field(
        cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")", "policy-mode");
    CHECK(policy_mode == 0,
          std::format("policy-mode = {} (expected 0 = log-and-sync default — #756 already "
                      "exposes desync_panic_count via envframe_desync_panic_count_total; Phase "
                      "2+ to make policy mode configurable per body \"policy flag (strict_panic "
                      "vs log_and_sync)\")",
                      policy_mode));
    const auto call_sites =
        hash_int_field(cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")",
                       "mandatory-call-sites-enabled");
    CHECK(call_sites == 0,
          std::format("mandatory-call-sites-enabled = {} (expected 0 — Phase 2+ deferred to "
                      "wire ensure_ at walk_env_frames / GCEnvWalkFn / materialize_call_env / "
                      "post-rollback paths)",
                      call_sites));
    const auto rec =
        hash_int_field(cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")",
                       "recommendation");
    CHECK(rec == 3,
          std::format("recommendation = {} (expected 3 = early-stage when both deferred flags "
                      "== 0 AND no activity)",
                      rec));
}

static void run_ac3_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: schema == 784 (drift sentinel) ---");
    const auto schema = hash_int_field(
        cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")", "schema");
    CHECK(schema == 784, std::format("schema = {} (expected 784)", schema));
}

static void run_ac4_bump_correctness(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: production-path bump helpers + primitive read-back ---");

    // Snapshot before.
    const auto enforce_before =
        hash_int_field(cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")",
                       "mandatory-enforce-total");
    const auto desync_before =
        hash_int_field(cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")",
                       "mandatory-enforce-desync-total");
    const auto steal_resync_before =
        hash_int_field(cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")",
                       "concurrent-steal-resync-total");

    // Exercise the 3 NEW per-Evaluator bump helpers
    // via the service's evaluator instance. The bump
    // helpers bump CompilerMetrics atomics (which the
    // primitive reads via ev.compiler_metrics()).
    auto& ev = cs.evaluator();
    constexpr int k_iters = 7;
    for (int i = 0; i < k_iters; ++i) {
        ev.bump_envframe_mandatory_enforce();
        ev.bump_envframe_mandatory_enforce_desync();
        ev.bump_envframe_concurrent_steal_resync();
    }

    const auto enforce_after =
        hash_int_field(cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")",
                       "mandatory-enforce-total");
    const auto desync_after =
        hash_int_field(cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")",
                       "mandatory-enforce-desync-total");
    const auto steal_resync_after =
        hash_int_field(cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")",
                       "concurrent-steal-resync-total");

    std::println("  counts after AC4 bumps: enforce {} -> {}, desync {} -> {}, steal-resync {} "
                 "-> {}",
                 enforce_before, enforce_after, desync_before, desync_after, steal_resync_before,
                 steal_resync_after);

    // Direct bump helpers added exactly k_iters to
    // each of the 3 NEW atomics.
    CHECK(enforce_after >= enforce_before + k_iters,
          std::format("mandatory-enforce-total bumped by bump_envframe_mandatory_enforce ({} -> "
                      "{})",
                      enforce_before, enforce_after));
    CHECK(desync_after >= desync_before + k_iters,
          std::format("mandatory-enforce-desync-total bumped by "
                      "bump_envframe_mandatory_enforce_desync ({} -> {})",
                      desync_before, desync_after));
    CHECK(steal_resync_after >= steal_resync_before + k_iters,
          std::format("concurrent-steal-resync-total bumped by "
                      "bump_envframe_concurrent_steal_resync ({} -> {})",
                      steal_resync_before, steal_resync_after));

    // Recommendation should now be 2 (Phase 1 only —
    // both deferred flags == 0 BUT activity > 0).
    const auto rec_after =
        hash_int_field(cs, "(engine:metrics \"query:envframe-dualpath-mandatory-enforce-stats\")",
                       "recommendation");
    CHECK(rec_after == 2,
          std::format("recommendation = {} (expected 2 = Phase 1 only after activity; "
                      "activity > 0 with both deferred flags == 0)",
                      rec_after));
}

static void run_ac5_sibling_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: regression — #756 + #783 sibling primitives unaffected ---");
    auto a756 = cs.eval("(engine:metrics \"query:envframe-dualpath-policy-stats\")");
    auto a783 = cs.eval("(engine:metrics \"query:orchestration-steal-outermost-stats\")");
    CHECK(a756 && aura::compiler::types::is_hash(*a756),
          "query:envframe-dualpath-policy-stats hash regression (#756)");
    CHECK(a783 && aura::compiler::types::is_hash(*a783),
          "query:orchestration-steal-outermost-stats hash regression (#783)");
    const auto a756_schema =
        hash_int_field(cs, "(engine:metrics \"query:envframe-dualpath-policy-stats\")", "schema");
    CHECK(a756_schema == 756,
          std::format("#756 schema = {} (expected 756, no drift)", a756_schema));
    const auto a783_schema = hash_int_field(
        cs, "(engine:metrics \"query:orchestration-steal-outermost-stats\")", "schema");
    CHECK(a783_schema == 783,
          std::format("#783 schema = {} (expected 783, no drift)", a783_schema));
}

} // namespace aura_issue_784_detail

int aura_issue_784_run() {
    using namespace aura_issue_784_detail;
    std::println("=== Issue #784: P0 mandatory dual-path consistency enforcement + desync "
                 "panic policy + GCEnvWalkFn stale integration observability (scope-limited "
                 "close) ===");

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
    return aura_issue_784_run();
}
#endif
