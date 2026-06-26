// @category: integration
// @reason: uses CompilerService to eval Aura source
// test_issue_169.cpp — Issue #169: Fine-grained Incremental
// Compilation v3 (Phase 1: config flag).
//
// Verifies the basic API works:
//   1. IncrementalStrictness enum is defined with 3 values
//   2. CompilerService has set/get methods for the flag
//   3. Default value is Balanced (no behavior change)
//   4. Setting Conservative/Aggressive works (reserves the values)
//   5. The build doesn't break (regression on the rest of the codebase)


// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.diag;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;



namespace aura_issue_169_detail {
#define PRINTLN(msg) do { std::print("{}\n", std::string(msg)); } while(0)

// ── Test 1: enum is defined with 3 distinct values ──
bool test_enum_values() {
    PRINTLN("\n--- Test 1: IncrementalStrictness enum has 3 distinct values ---");
    CHECK(static_cast<std::uint8_t>(aura::compiler::IncrementalStrictness::Conservative) == 0,
          "Conservative = 0");
    CHECK(static_cast<std::uint8_t>(aura::compiler::IncrementalStrictness::Balanced) == 1,
          "Balanced = 1");
    CHECK(static_cast<std::uint8_t>(aura::compiler::IncrementalStrictness::Aggressive) == 2,
          "Aggressive = 2");
    // Distinct values
    CHECK(aura::compiler::IncrementalStrictness::Conservative !=
          aura::compiler::IncrementalStrictness::Balanced,
          "Conservative != Balanced");
    CHECK(aura::compiler::IncrementalStrictness::Balanced !=
          aura::compiler::IncrementalStrictness::Aggressive,
          "Balanced != Aggressive");
    return true;
}

// ── Test 2: CompilerService has set/get methods ──
bool test_set_get() {
    PRINTLN("\n--- Test 2: CompilerService set/get methods ---");
    aura::compiler::CompilerService cs;
    // Default is Balanced
    CHECK(cs.incremental_strictness() == aura::compiler::IncrementalStrictness::Balanced,
          "default strictness is Balanced");
    // Set to Conservative
    cs.set_incremental_strictness(aura::compiler::IncrementalStrictness::Conservative);
    CHECK(cs.incremental_strictness() == aura::compiler::IncrementalStrictness::Conservative,
          "set to Conservative, getter returns Conservative");
    // Set to Aggressive
    cs.set_incremental_strictness(aura::compiler::IncrementalStrictness::Aggressive);
    CHECK(cs.incremental_strictness() == aura::compiler::IncrementalStrictness::Aggressive,
          "set to Aggressive, getter returns Aggressive");
    // Back to Balanced
    cs.set_incremental_strictness(aura::compiler::IncrementalStrictness::Balanced);
    CHECK(cs.incremental_strictness() == aura::compiler::IncrementalStrictness::Balanced,
          "set back to Balanced");
    return true;
}

// ── Test 3: setting the flag doesn't crash eval (no behavior change) ──
bool test_no_behavior_change() {
    PRINTLN("\n--- Test 3: setting the flag doesn't change eval behavior ---");
    aura::compiler::CompilerService cs;
    cs.set_incremental_strictness(aura::compiler::IncrementalStrictness::Conservative);
    auto r = cs.eval("(+ 1 2)");
    CHECK(r.has_value(), "Conservative mode: eval(+ 1 2) returns a value");
    cs.set_incremental_strictness(aura::compiler::IncrementalStrictness::Aggressive);
    r = cs.eval("(+ 3 4)");
    CHECK(r.has_value(), "Aggressive mode: eval(+ 3 4) returns a value");
    return true;
}

int run_tests() {
    std::println("═══ Issue #169 — Incremental Compilation v3 (Phase 1: config flag) ═══");

    test_enum_values();
    test_set_get();
    test_no_behavior_change();

    std::println("\n──────────────────────────────────────");
    std::println("Total: %d passed, %d failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_169_detail

int aura_issue_169_run() { return aura_issue_169_detail::run_tests(); }

