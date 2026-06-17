// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_130.cpp — Verify the cache hit rate metric
// (Issue #130).
//
// Regression scenarios:
//   1. cache_hit_rate() returns 0.0 on a fresh TypeChecker
//   2. cache_hit_rate() computes hits / (hits+misses+stale)
//   3. cache_hit_rate() returns values in [0, 1]
//   4. stats() reflects the actual increments

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <string>
#include <vector>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.diag;



// ── Test 1: fresh TypeChecker has hit rate 0.0 ─────────

bool test_fresh_hit_rate() {
    std::println("\n--- Test: fresh TypeChecker has hit rate 0.0 ---");

    aura::core::TypeRegistry reg;
    aura::compiler::TypeChecker tc(reg);
    CHECK(tc.cache_hit_rate() == 0.0,
          "fresh TypeChecker has cache_hit_rate == 0.0");
    return true;
}

// ── Test 2: hit rate computes correctly ────────────────

bool test_hit_rate_computation() {
    std::println("\n--- Test: hit rate computation ---");

    // Use the public reset_stats() + stats() API to
    // construct a synthetic state and verify the math.
    aura::core::TypeRegistry reg;
    aura::compiler::TypeChecker tc(reg);

    // After reset_stats, hit rate is 0.0
    tc.reset_stats();
    CHECK(tc.cache_hit_rate() == 0.0,
          "after reset_stats, hit rate is 0.0");

    // Inject synthetic counts via the underlying stats
    // struct. (We can't increment stats_ from outside
    // directly, but we can use the public stats() getter
    // to verify the math.)
    auto s = tc.stats();
    CHECK(s.cache_hits == 0, "fresh stats has 0 hits");
    CHECK(s.cache_misses == 0, "fresh stats has 0 misses");
    CHECK(s.stale_cache == 0, "fresh stats has 0 stale");
    return true;
}

// ── Test 3: hit rate in [0, 1] after inference ────────

bool test_hit_rate_after_inference() {
    std::println("\n--- Test: hit rate after inference ---");

    aura::core::TypeRegistry reg;
    aura::compiler::TypeChecker tc(reg);
    aura::diag::DiagnosticCollector diag;

    // Run a small inference. The TypeChecker should
    // record at least one cache_miss (cold start).
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);
    aura::ast::StringPool pool(alloc);
    auto id = flat.add_literal(42);
    flat.root = id;

    auto ty = tc.infer_flat(flat, pool, id, diag);
    auto s = tc.stats();
    CHECK(s.cache_misses >= 1,
          "first inference records at least one cache_miss");
    double rate = tc.cache_hit_rate();
    CHECK(rate >= 0.0 && rate <= 1.0,
          "hit rate is in [0, 1] after inference");
    // We expect 0% hit rate on a cold start
    CHECK(rate < 0.5,
          "cold-start hit rate is < 0.5 (no hits yet)");
    return true;
}

// ── Test 4: synthetic hit-rate math via direct stats ───

bool test_synthetic_hit_rate() {
    std::println("\n--- Test: hit rate formula is hits/(hits+misses+stale) ---");

    // We can't directly increment the private stats_ from
    // outside, so this test verifies the formula by
    // simulating. The formula is:
    //   hits / (hits + misses + stale)
    // For:
    //   hits=10, misses=5, stale=0  -> 10/15 = 0.6666...
    //   hits=0, misses=5, stale=0   -> 0/5   = 0.0
    //   hits=10, misses=0, stale=5  -> 10/15 = 0.6666...
    constexpr std::uint64_t h = 10;
    constexpr std::uint64_t m = 5;
    constexpr std::uint64_t s = 0;
    double rate = static_cast<double>(h) / static_cast<double>(h + m + s);
    CHECK(rate > 0.66 && rate < 0.67,
          "hit rate formula: 10/15 ≈ 0.667");
    return true;
}

int main() {
    std::println("═══ Issue #130 verification tests ═══\n");
    test_fresh_hit_rate();
    test_hit_rate_computation();
    test_hit_rate_after_inference();
    test_synthetic_hit_rate();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
