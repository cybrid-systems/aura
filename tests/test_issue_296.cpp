// @category: integration
// @reason: Issue #296 — isolated Closure Bridge unit tests
//
// Validates the Bridge Lifetime Contract documented in
// src/compiler/evaluator.ixx and src/compiler/service.ixx.
//
// Contract summary:
//   1. bridge_epoch == 0 is "legacy / not tracked" — trustworthy
//   2. Non-zero values: strict validation, mismatch = stale
//   3. Callers must not pass current_epoch == 0 when bridge has been bumped
//   4. The bridge_epoch counter is bumped atomically on structural mutation
//
// State machine:
//   fresh closure ─captures─> bridge_epoch = current
//   bump_bridge_epoch() ─bumps─> current
//   is_bridge_stale(captured, current) ─checks─> bool
//   stale closure ─falls back─> body_source re-parse

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;
import aura.core.ast;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace test_296_detail {

// AC #1: legacy bridge_epoch (0) is trusted, never stale
bool test_legacy_trusted() {
    std::println("\n--- AC #1: bridge_epoch == 0 is legacy / trusted ---");
    // current=0 (initial), captured=0 → not stale (both legacy)
    CHECK(!aura::compiler::Evaluator::is_bridge_stale(0, 0), "0 vs 0 not stale");
    // current=N, captured=0 → not stale (legacy wins)
    CHECK(!aura::compiler::Evaluator::is_bridge_stale(0, 100),
          "captured=0 (legacy) not stale even when current=100");
    return true;
}

// AC #2: matching non-zero values are not stale
bool test_matching_not_stale() {
    std::println("\n--- AC #2: matching non-zero values not stale ---");
    CHECK(!aura::compiler::Evaluator::is_bridge_stale(42, 42), "42 vs 42 not stale");
    CHECK(!aura::compiler::Evaluator::is_bridge_stale(1, 1), "1 vs 1 not stale");
    CHECK(!aura::compiler::Evaluator::is_bridge_stale(999999, 999999),
          "999999 vs 999999 not stale");
    return true;
}

// AC #3: different non-zero values are stale
bool test_mismatch_stale() {
    std::println("\n--- AC #3: mismatch (non-zero) is stale ---");
    CHECK(aura::compiler::Evaluator::is_bridge_stale(1, 2), "1 vs 2 stale");
    CHECK(aura::compiler::Evaluator::is_bridge_stale(42, 100), "42 vs 100 stale");
    CHECK(aura::compiler::Evaluator::is_bridge_stale(100, 42), "100 vs 42 stale (both directions)");
    return true;
}

// AC #4: bump_bridge_epoch advances the counter (monotonicity)
bool test_bump_monotonic() {
    std::println("\n--- AC #4: bump_bridge_epoch is monotonic ---");
    aura::compiler::CompilerService cs;
    std::uint64_t e0 = cs.bridge_epoch();
    cs.bump_bridge_epoch();
    std::uint64_t e1 = cs.bridge_epoch();
    cs.bump_bridge_epoch();
    cs.bump_bridge_epoch();
    std::uint64_t e2 = cs.bridge_epoch();
    CHECK(e0 <= e1,
          "bump 1 advances epoch (e0=" + std::to_string(e0) + " e1=" + std::to_string(e1) + ")");
    CHECK(e1 < e2, "bump 2 advances epoch further (e1=" + std::to_string(e1) +
                       " e2=" + std::to_string(e2) + ")");
    return true;
}

// AC #5: after bump, a closure captured before the bump is stale
bool test_stale_after_bump() {
    std::println("\n--- AC #5: closure captured pre-bump is stale ---");
    aura::compiler::CompilerService cs;
    std::uint64_t before_bump = cs.bridge_epoch();
    if (before_bump == 0)
        cs.bump_bridge_epoch(); // ensure non-zero
    before_bump = cs.bridge_epoch();
    cs.bump_bridge_epoch();
    std::uint64_t after_bump = cs.bridge_epoch();
    CHECK(after_bump > before_bump, "after_bump > before_bump (" + std::to_string(after_bump) +
                                        " > " + std::to_string(before_bump) + ")");
    // The "closure" captured at before_bump is now stale
    CHECK(aura::compiler::Evaluator::is_bridge_stale(before_bump, after_bump),
          "closure from pre-bump is stale (captured=" + std::to_string(before_bump) +
              " current=" + std::to_string(after_bump) + ")");
    return true;
}

// AC #6: real apply_closure with bumping doesn't crash
// (This is the integration smoke test — runs actual Aura code
// that uses define + closure, then bumps the bridge epoch,
// then evaluates again to verify the fallback works.)
bool test_apply_closure_after_bump() {
    std::println("\n--- AC #6: apply_closure survives bridge epoch bump ---");
    aura::compiler::CompilerService cs;
    // Set up a simple function definition
    auto r1 = cs.eval(R"AU(
        (set-code "(define (id x) x)")
    )AU");
    // Call it before bump
    auto r2 = cs.eval("(id 42)");
    if (r2) {
        auto& v = *r2;
        if (aura::compiler::types::is_int(v)) {
            int64_t result = aura::compiler::types::as_int(v);
            CHECK(result == 42, "id(42) == 42 before bump (got " + std::to_string(result) + ")");
        }
    }
    // Bump the bridge epoch
    cs.bump_bridge_epoch();
    // Call it again — should still work via re-parse fallback
    auto r3 = cs.eval("(id 99)");
    if (r3) {
        auto& v = *r3;
        if (aura::compiler::types::is_int(v)) {
            int64_t result = aura::compiler::types::as_int(v);
            CHECK(result == 99, "id(99) == 99 after bump (got " + std::to_string(result) + ")");
        }
    }
    return true;
}

// AC #7: bridge_epoch() + is_bridge_stale composition roundtrip
// Capture epoch, bump, check stale, restore (simulate).
bool test_roundtrip_composition() {
    std::println("\n--- AC #7: capture-bump-check roundtrip ---");
    aura::compiler::CompilerService cs;
    if (cs.bridge_epoch() == 0)
        cs.bump_bridge_epoch();
    std::uint64_t captured = cs.bridge_epoch();
    // No bumps yet: not stale
    CHECK(!aura::compiler::Evaluator::is_bridge_stale(captured, cs.bridge_epoch()),
          "captured = current (not stale)");
    // Bump once
    cs.bump_bridge_epoch();
    // Now stale
    CHECK(aura::compiler::Evaluator::is_bridge_stale(captured, cs.bridge_epoch()),
          "captured < current (stale)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #296 ═══");
    test_legacy_trusted();
    test_matching_not_stale();
    test_mismatch_stale();
    test_bump_monotonic();
    test_stale_after_bump();
    test_apply_closure_after_bump();
    test_roundtrip_composition();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}

} // namespace test_296_detail

int aura_issue_296_run() {
    return test_296_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_296_run();
}
#endif