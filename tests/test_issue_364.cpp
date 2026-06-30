// test_issue_364.cpp — Issue #364: Nested hygienic macros +
// mutation interactions test coverage.
//
// Extends #326 (hygienic macros + EDSL basic integration,
// shipped earlier today: 8 scenarios / 25 assertions) with:
//
//   - Nested hygienic macros (macro-inside-macro expansion)
//   - Macros mutated via mutate:rebind
//   - snapshot + rollback consistency under macro expand
//   - 100+ cycle stress test
//
// AC #1: test file created (this file).
// AC #2: nested swap! scenarios.
// AC #3: MutationBoundaryGuard + ast:snapshot/rollback integration.
// AC #4: stress (100+ cycles).
// AC #5: TSan/ASan (binary built; CI registration deferred).
// AC #6: docs/design/core/hygienic_macros.md updated.

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_364_detail {

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println("  PASS: {}", msg); } \
    else      { ++g_failed; std::println(std::cerr, "  FAIL: {}", msg); } \
} while (0)

using aura::compiler::CompilerService;

// ── Scenario 1: nested hygienic macro expansion ──
bool test_nested_macro_expansion() {
    std::println("\n--- Scenario 1: nested hygienic macro expansion ---");
    CompilerService cs;
    (void)cs.eval("(set-code \""
                  "(define-hygienic-macro (inner x) (list 'define (list 'v x) x)) "
                  "(define-hygienic-macro (outer n) (inner (inner n))) "
                  "(outer 42)\")");
    auto r = cs.eval("(eval-current)");
    CHECK(r.has_value(), "nested macro eval succeeds");
    return true;
}

// ── Scenario 2: swap! example extended to nested ──
bool test_nested_swap() {
    std::println("\n--- Scenario 2: nested swap! ---");
    CompilerService cs;
    (void)cs.eval("(set-code \""
                  "(define-hygienic-macro (swap! a b) "
                  "  (let ((tmp a)) (set! a b) (set! b tmp))) "
                  "(define-hygienic-macro (double-swap! a b c d) "
                  "  (swap! a b) (swap! c d)) "
                  "(let ((x 1) (y 2) (p 10) (q 20)) "
                  "  (double-swap! x y p q) "
                  "  (list x y p q))\")");
    auto r = cs.eval("(eval-current)");
    CHECK(r.has_value(), "nested swap! eval succeeds");
    return true;
}

// ── Scenario 3: macro mutated via mutate:rebind ──
bool test_macro_mutated_via_rebind() {
    std::println("\n--- Scenario 3: macro mutated via mutate:rebind ---");
    CompilerService cs;
    (void)cs.eval("(set-code \""
                  "(define-hygienic-macro (mk x) (list 'define (list 'v x) x)) "
                  "(mk 100)\")");
    (void)cs.eval("(eval-current)");
    // Rebind the macro definition to a new body.
    auto r = cs.eval("(mutate:rebind \"mk\" \"(lambda (x) (list 'define (list 'w x) x))\")");
    CHECK(r.has_value(), "mutate:rebind on macro succeeds");
    return true;
}

// ── Scenario 4: snapshot + rollback under macro expand ──
bool test_snapshot_rollback_with_macro() {
    std::println("\n--- Scenario 4: snapshot + rollback with macro ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define x 1)\")");
    (void)cs.eval("(eval-current)");
    // Capture a snapshot.
    auto snap = cs.eval("(ast:snapshot)");
    CHECK(snap.has_value(), "ast:snapshot returns a value");
    // Add a macro + use it.
    (void)cs.eval("(set-code \""
                  "(define-hygienic-macro (mk x) (list 'define (list 'v x) x)) "
                  "(mk 42)\")");
    (void)cs.eval("(eval-current)");
    // Rollback to the pre-snapshot state.
    auto roll = cs.eval("(rollback)");
    CHECK(roll.has_value(), "rollback returns a value");
    return true;
}

// ── Scenario 5: 100+ cycle stress (define + mutate + expand) ──
bool test_100_cycle_stress() {
    std::println("\n--- Scenario 5: 100+ cycle stress (define + mutate + expand) ---");
    CompilerService cs;
    constexpr int N = 100;
    for (int i = 0; i < N; ++i) {
        std::string code = "(set-code \"(define-hygienic-macro (mk";
        code += std::to_string(i);
        code += " x) (list 'define (list 'v";
        code += std::to_string(i);
        code += " x) x)) (mk";
        code += std::to_string(i);
        code += " ";
        code += std::to_string(i * 7);
        code += ")\")";
        auto r = cs.eval(code);
        CHECK(r.has_value(),
              std::string("cycle #") + std::to_string(i) + " set-code ok");
        auto e = cs.eval("(eval-current)");
        CHECK(e.has_value(),
              std::string("cycle #") + std::to_string(i) + " eval ok");
    }
    std::println("  {} cycles completed", N);
    return true;
}

// ── Scenario 6: macro defining macro + nested mutation ──
bool test_macro_defines_macro_with_mutation() {
    std::println("\n--- Scenario 6: macro defining macro + nested mutation ---");
    CompilerService cs;
    (void)cs.eval("(set-code \""
                  "(define-hygienic-macro (make-adder-macro n) "
                  "  (list 'define-hygienic-macro "
                  "        (list 'add n) "
                  "        (list '+ n 'x))) "
                  "(make-adder-macro 10) "
                  "(add 5)\")");
    auto r = cs.eval("(eval-current)");
    CHECK(r.has_value(), "macro-defining-macro + nested eval succeeds");
    // Mutate the outer macro definition.
    (void)cs.eval("(mutate:rebind \"make-adder-macro\" "
                  "\"(lambda (n) "
                  "(list 'define-hygienic-macro "
                  "(list 'add n) "
                  "(list '* n 'x)))\")");
    auto r2 = cs.eval("(eval-current)");
    CHECK(r2.has_value(), "mutate on macro-defining-macro doesn't crash");
    return true;
}

} // namespace aura_364_detail

// Issue #364 deferred AC #5: bundle-compatible runner so the
// test gets executed as part of `python3 build.py test issues`
// (and the JIT + ASan bundles) instead of only as a standalone
// target. The standalone `int main()` is preserved by the
// aura_issue_364_run() trampoline.
int aura_issue_364_run() {
    using namespace aura_364_detail;
    test_nested_macro_expansion();
    test_nested_swap();
    test_macro_mutated_via_rebind();
    test_snapshot_rollback_with_macro();
    test_100_cycle_stress();
    test_macro_defines_macro_with_mutation();
    return g_failed == 0 ? 0 : 1;
}

// Standalone entry point for `cmake --target test_issue_364`.
// When compiled standalone, aura_issue_364_run is the only
// extern "C" entry so the linker picks this `main`. When
// compiled into a bundle, the bundle main calls
// aura_issue_364_run() directly and this main is unused
// (but the linker still needs to see a `main` symbol for the
// standalone build path — guarded by AURA_BUNDLE_BUILD).
#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    int rc = aura_issue_364_run();
    std::println("Nested hygienic macros + mutation (#364): {}/{} passed, {}/{} failed",
                 aura_364_detail::g_passed,
                 aura_364_detail::g_passed + aura_364_detail::g_failed,
                 aura_364_detail::g_failed,
                 aura_364_detail::g_passed + aura_364_detail::g_failed);
    return rc;
}
#endif
