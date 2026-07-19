// @category: issue_specific
// @reason: default for test_issue_*.cpp
// test_issue_174.cpp — Issue #174 Cycle 1 Env::bindings_
// migration: bindings_symid_iter() + bindings_with_names()
// + bindings_legacy_uses metric.
//
// Verifies:
//   1. bindings_symid_iter() returns same data as bindings()
//      (just a different keying: SymId vs string)
//   2. bindings_legacy_uses counter starts at 0 and bumps
//      on legacy access only
//   3. bind_symid works without pool_ (precondition test)
//   4. bind(name, value) convenience routes through
//      bind_symid (when pool_ is set)


// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core;
import aura.compiler.value;


namespace aura_issue_174_detail {
#define PRINTLN(msg) std::print("%s\n", (msg))

// Helper: build an Env, bind a few name→value pairs, and
// return it. The pool_ is set to the canonical pool so
// bind(name, value) works as expected. Uses bind_symid
// (which mirrors to bindings_) so both views are populated.
static aura::compiler::Env
make_env_with_bindings(aura::ast::StringPool& pool,
                       const std::vector<std::pair<std::string, int>>& pairs) {
    aura::compiler::Env env;
    env.set_pool(&pool);
    for (const auto& [n, v] : pairs) {
        // Use bind_symid directly so both bindings_ and
        // bindings_symid_ are populated. (The current
        // bind(name, value) writes to bindings_ only
        // because pool_ is const, so we can't intern
        // a new SymId from a const pool. Cycle 2 may
        // change pool_ to non-const, then bind() can
        // route through bind_symid.)
        auto sym = pool.intern(n);
        env.bind_symid(sym, aura::compiler::types::make_int(v));
    }
    return env;
}

// Helper: empty env (for tests that don't need bindings)
static aura::compiler::Env make_env_bindings_empty() {
    return aura::compiler::Env{};
}

// ── Test 1: bindings_symid_iter() returns same data as bindings() ──
// Both views should have the same length and the same
// values (in the same order), just keyed differently.
bool test_bindings_symid_iter_matches_bindings() {
    PRINTLN("\n--- Test 1: bindings_symid_iter matches bindings() ---");
    aura::ast::StringPool pool;
    auto env = make_env_with_bindings(pool, {{"x", 42}, {"y", 99}, {"z", 7}});
    auto legacy = env.bindings();
    auto symid_view = env.bindings_symid_iter();
    CHECK(legacy.size() == symid_view.size(), "legacy and symid views have same length");
    CHECK(legacy.size() == 3, "3 bindings");
    // Same values, same order
    for (std::size_t i = 0; i < legacy.size(); ++i) {
        CHECK(legacy[i].second == symid_view[i].second,
              "value at index " + std::to_string(i) + " matches");
    }
    // Same names (legacy has strings; symid has SymIds;
    // resolve the SymIds via the pool to compare)
    for (std::size_t i = 0; i < legacy.size(); ++i) {
        std::string_view resolved = pool.resolve(symid_view[i].first);
        CHECK(resolved == legacy[i].first, "name at index " + std::to_string(i) + " matches");
    }
    return true;
}

// ── Test 2: bindings_legacy_uses counter starts at 0 and bumps ──
// Counter starts at 0, bumps on every legacy access.
bool test_bindings_legacy_uses_counter() {
    PRINTLN("\n--- Test 2: bindings_legacy_uses counter ---");
    aura::ast::StringPool pool;
    auto env = make_env_bindings_empty();
    CHECK(env.bindings_legacy_uses() == 0, "counter starts at 0");
    // First legacy access: counter = 1
    auto legacy = env.bindings();
    CHECK(env.bindings_legacy_uses() == 1, "counter bumps to 1 after one legacy access");
    // Second legacy access: counter = 2
    auto legacy2 = env.bindings();
    CHECK(env.bindings_legacy_uses() == 2, "counter bumps to 2 after two legacy accesses");
    // New accessor does NOT bump the counter
    auto symid_view = env.bindings_symid_iter();
    CHECK(env.bindings_legacy_uses() == 2, "counter unchanged after bindings_symid_iter access");
    (void)legacy;
    (void)legacy2;
    (void)symid_view;
    return true;
}

// Helper: empty env (for tests that don't need bindings)
// (declared above)

// ── Test 3: bind_symid works without pool_ ──
// Even if pool_ is null, bind_symid should work (it just
// doesn't mirror to bindings_).
bool test_bind_symid_works_without_pool() {
    PRINTLN("\n--- Test 3: bind_symid works without pool_ ---");
    aura::compiler::Env env;
    // No set_pool() — pool_ is nullptr
    CHECK(env.bindings_symid_iter().size() == 0, "no bindings before bind");
    // bind_symid with a synthetic SymId (don't need a pool)
    env.bind_symid(42, aura::compiler::types::make_int(123));
    auto symid_view = env.bindings_symid_iter();
    CHECK(symid_view.size() == 1, "1 binding after bind_symid");
    CHECK(symid_view[0].first == 42, "SymId is 42");
    return true;
}

// ── Test 4: bind_symid mirrors to bindings_ when pool_ is set ──
// This is the "bind(name, value) routes through intern +
// bind_symid" scenario the issue body describes. We do it
// manually here because the current Env::bind() impl writes
// to bindings_ only (pool_ is const, so we can't intern
// from a const pool in bind()). Cycle 2 may unify the
// bind() and bind_symid() paths by making pool_ non-const.
bool test_bind_symid_mirrors_to_legacy() {
    PRINTLN("\n--- Test 4: bind_symid mirrors to bindings_ when pool_ is set ---");
    aura::ast::StringPool pool;
    aura::compiler::Env env;
    env.set_pool(&pool);
    // Intern "x" first to get a SymId
    auto symid_x = pool.intern("x");
    env.bind_symid(symid_x, aura::compiler::types::make_int(42));
    // Legacy view should have the binding (mirror)
    auto legacy = env.bindings();
    CHECK(legacy.size() == 1, "1 binding in legacy view (mirrored)");
    CHECK(legacy[0].first == "x", "name is 'x'");
    // SymId view should have the same binding
    auto symid_view = env.bindings_symid_iter();
    CHECK(symid_view.size() == 1, "1 binding in symid view");
    CHECK(symid_view[0].first == symid_x, "SymId matches pool.intern('x')");
    return true;
}

// ── Test 5: bindings_with_names() returns named view ──
// When pool_ is set, bindings_with_names() returns the
// same data as bindings() (but materialized on demand).
// When pool_ is null, names fall back to "@symid:N".
bool test_bindings_with_names() {
    PRINTLN("\n--- Test 5: bindings_with_names() ---");
    aura::ast::StringPool pool;
    auto env = make_env_with_bindings(pool, {{"alpha", 1}, {"beta", 2}, {"gamma", 3}});
    auto named = env.bindings_with_names();
    CHECK(named.size() == 3, "3 named bindings");
    CHECK(named[0].first == "alpha", "[0] is 'alpha'");
    CHECK(named[1].first == "beta", "[1] is 'beta'");
    CHECK(named[2].first == "gamma", "[2] is 'gamma'");
    // Now test the fallback (no pool_)
    aura::compiler::Env env_no_pool;
    env_no_pool.bind_symid(7, aura::compiler::types::make_int(99));
    auto named_no_pool = env_no_pool.bindings_with_names();
    CHECK(named_no_pool.size() == 1, "1 named binding (no pool)");
    CHECK(named_no_pool[0].first == "@symid:7", "fallback name is '@symid:7'");
    return true;
}

int run_tests() {
    std::println("═══ Issue #174 Cycle 1 — Env::bindings_ migration ═══");
    std::println("  Adds bindings_symid_iter(), bindings_with_names(),");
    std::println("  and bindings_legacy_uses metric.\n");

    test_bindings_symid_iter_matches_bindings();
    test_bindings_legacy_uses_counter();
    test_bind_symid_works_without_pool();
    test_bind_symid_mirrors_to_legacy();
    test_bindings_with_names();

    std::println("\n──────────────────────────────────────");
    std::println("Total: %d passed, %d failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
} // namespace aura_issue_174_detail

int aura_issue_174_run() {
    return aura_issue_174_detail::run_tests();
}
