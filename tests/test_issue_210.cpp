// @category: issue_specific
// @reason: default for test_issue_*.cpp
// test_issue_210.cpp — Issue #210 Cycle 4 env cleanup:
// the final step (drop bindings_ entirely).
//
// The full Cycle 4 cleanup requires ALL callers of the
// legacy Env::bindings() accessor to be migrated first
// (per the Cycle 2.5+ plan documented in #208). This
// test verifies the cleanup-readiness:
//
//   1. An env populated only via bind_symid (no
//      bindings() access) has bindings_legacy_uses == 0.
//   2. bindings_with_names() (the new accessor) does
//      not bump the metric.
//   3. The legacy bindings() accessor is still functional
//      (it just bumps the metric; no behavior change).
//   4. The new code path (bindings_symid_iter) produces
//      the same data as the legacy path (data parity).
//
// The actual `drop bindings_` commit is deferred to
// after the Cycle 2.5+ migration is complete. This PR
// documents the cleanup path and adds tests that
// exercise the post-migration invariants.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core;
import aura.compiler.value;



namespace aura_issue_210_detail {
#define PRINTLN(msg) std::fprintf(stdout, "%s\n", (msg))

// ── Test 1: env populated via bind_symid has bindings_legacy_uses == 0 ──
// The new path (bind_symid, no legacy access) leaves the
// metric at 0. This is the post-Cycle-4 invariant: the
// env works without the legacy `bindings_` field being
// populated.
bool test_bind_symid_path_keeps_metric_zero() {
    PRINTLN("\n--- Test 1: bind_symid path keeps bindings_legacy_uses at 0 ---");
    aura::compiler::Env env;
    aura::ast::StringPool pool;
    env.set_pool(&pool);
    env.bind_symid(pool.intern("x"), aura::compiler::types::make_int(1));
    env.bind_symid(pool.intern("y"), aura::compiler::types::make_int(2));
    CHECK(env.bindings_legacy_uses() == 0,
          "bind_symid does NOT bump bindings_legacy_uses (the metric tracks the accessor, not the field)");
    // The env is fully usable via the new accessors.
    auto view = env.bindings_symid_iter();
    CHECK(view.size() == 2, "2 bindings via bindings_symid_iter");
    auto named = env.bindings_with_names();
    CHECK(named.size() == 2, "2 bindings via bindings_with_names");
    CHECK(named[0].first == "x", "[0] is 'x'");
    CHECK(named[1].first == "y", "[1] is 'y'");
    // Metric still 0 after using the new accessors.
    CHECK(env.bindings_legacy_uses() == 0,
          "metric still 0 after using new accessors");
    return true;
}

// ── Test 2: legacy bindings() accessor still works ──
// The accessor remains functional (no behavior change).
// The metric bumps, which is the migration observability
// signal.
bool test_legacy_bindings_accessor_still_works() {
    PRINTLN("\n--- Test 2: legacy bindings() accessor still works ---");
    aura::compiler::Env env;
    aura::ast::StringPool pool;
    env.set_pool(&pool);
    env.bind_symid(pool.intern("foo"), aura::compiler::types::make_int(42));
    env.bind_symid(pool.intern("bar"), aura::compiler::types::make_int(99));
    CHECK(env.bindings_legacy_uses() == 0, "counter starts at 0");
    auto legacy = env.bindings();
    CHECK(env.bindings_legacy_uses() == 1,
          "counter bumps to 1 after one legacy access");
    // The legacy accessor returns the same data as the
    // new one (data parity).
    auto view = env.bindings_symid_iter();
    CHECK(legacy.size() == view.size(), "same number of bindings");
    for (std::size_t i = 0; i < legacy.size(); ++i) {
        CHECK(legacy[i].second == view[i].second,
              "value at index " + std::to_string(i) + " matches");
    }
    return true;
}

// ── Test 3: data parity between bindings_with_names and bindings() ──
// Both views return the same data after bind_symid
// populates both arrays. The new view doesn't bump the
// metric.
bool test_data_parity_with_pool() {
    PRINTLN("\n--- Test 3: data parity between bindings_with_names and bindings() ---");
    aura::compiler::Env env;
    aura::ast::StringPool pool;
    env.set_pool(&pool);
    env.bind_symid(pool.intern("a"), aura::compiler::types::make_int(10));
    env.bind_symid(pool.intern("b"), aura::compiler::types::make_int(20));
    env.bind_symid(pool.intern("c"), aura::compiler::types::make_int(30));
    auto named = env.bindings_with_names();
    auto legacy = env.bindings();
    CHECK(named.size() == legacy.size(), "same size");
    CHECK(named.size() == 3, "3 bindings");
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(named[i].first == legacy[i].first,
              "name at index " + std::to_string(i) + " matches");
        CHECK(named[i].second == legacy[i].second,
              "value at index " + std::to_string(i) + " matches");
    }
    // bindings_with_names doesn't bump the metric.
    // bindings() does. So after the loop above:
    // - 1 bump for `auto legacy = env.bindings();` (Test 2
    //   is a separate env, this is a new env)
    // - named accessor: 0 bumps
    // - legacy accessor: 1 bump
    // Net: 1 bump after the bindings() call.
    CHECK(env.bindings_legacy_uses() == 1,
          "bindings() bumped the counter once; named did not");
    return true;
}

// ── Test 4: env without pool_ uses '@symid:N' fallback ──
// This is the post-Cycle-4 invariant: an env that was
// never given a pool_ still works via the new accessors
// (with the '@symid:N' fallback for names).
bool test_env_without_pool_uses_fallback() {
    PRINTLN("\n--- Test 4: env without pool_ uses '@symid:N' fallback ---");
    aura::compiler::Env env;  // no set_pool
    env.bind_symid(42, aura::compiler::types::make_int(123));
    auto named = env.bindings_with_names();
    CHECK(named.size() == 1, "1 binding");
    CHECK(named[0].first == "@symid:42",
          "name is '@symid:42' (fallback for no-pool env)");
    // This binding is visible via bindings_symid_iter (the
    // canonical accessor) but NOT via the legacy
    // bindings() accessor (which uses bindings_, which is
    // empty because bind_symid didn't mirror — the
    // mirror only happens when pool_ is set).
    auto view = env.bindings_symid_iter();
    CHECK(view.size() == 1, "1 binding via symid view");
    // (We don't call env.bindings() here because the
    //  legacy accessor would return an empty span for an
    //  env where bindings_ wasn't mirrored. The Cycle 4
    //  drop of bindings_ would make this a hard error,
    //  but for now it's just a discrepancy.)
    return true;
}

int run_tests() {
    std::fprintf(stdout, "═══ Issue #210 Cycle 4 — Env::bindings_ cleanup ═══\n");
    std::fprintf(stdout, "  Verifies the cleanup-readiness invariants.\n");
    std::fprintf(stdout, "  The full drop of bindings_ is deferred to\n");
    std::fprintf(stdout, "  after the Cycle 2.5+ migration completes.\n\n");

    test_bind_symid_path_keeps_metric_zero();
    test_legacy_bindings_accessor_still_works();
    test_data_parity_with_pool();
    test_env_without_pool_uses_fallback();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_210_detail

int aura_issue_210_run() { return aura_issue_210_detail::run_tests(); }

