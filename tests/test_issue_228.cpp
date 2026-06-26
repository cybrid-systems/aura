// @category: integration
// test_issue_228.cpp — Issue #228: Hardware IR Dependent Type
// System for Agentic EDA (Phase 1 minimum viable).
//
// Phase 1 scope: BitVec + ClockDomain + ResetType + TypedSignal +
// compatibility checks. Tests run the Aura-level dependent type
// library (lib/std/eda.aura Phase 1 extension) end-to-end through
// the CompilerService eval path.
//
// Phase 1 deliverables (covered here):
//   1. BitVec with explicit integer width
//   2. BitVec with parametric (symbol) width (deferred resolution)
//   3. BitVec compatibility — equal widths
//   4. BitVec compatibility — mismatched widths
//   5. BitVec compatibility — parametric returns 'parametric
//   6. ClockDomain compatibility — same name → #t
//   7. ClockDomain compatibility — different names → #f
//   8. ResetType compatibility — same kind → #t
//   9. ResetType compatibility — different kinds → #f
//   10. TypedSignal compatibility — all three types match → #t
//   11. TypedSignal compatibility — width mismatch → #f
//   12. Convenience constructors (make-eda:bitvec-{8,16,32,64})
//
// Phases 2-5 (deferred to follow-up issues):
//   - Phase 2: Clock-domain + reset phantom type integration
//     with mutable primitives
//   - Phase 3: Protocol-aware transformations (Handshake, AXI)
//   - Phase 4: mutate:* primitives that call
//     eda:typed-signal-compatible? before allowing mutations
//   - Phase 5: Constraint solver for parametric widths


// Unified test harness (Issue #226 cycle 1+2).
#include "test_harness.hpp"

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.ir_executor;
import aura.compiler.evaluator;
import aura.compiler.service;

using aura::test::g_passed;
using aura::test::g_failed;

// Helper: run an Aura expression and return the result as
// a string representation. Used to read back Aura values
// (#t/#f/numbers/symbols) from the eval path.
namespace aura_issue_228_detail {
static std::string aura_eval_str(aura::compiler::CompilerService& cs,
                                 const std::string& src) {
    auto r = cs.eval(src);
    if (!r) return "<error>";
    return "<value>";
}

// Helper: eval and check that the result is #t
static bool eval_is_true(aura::compiler::CompilerService& cs,
                         const std::string& src) {
    auto r = cs.eval(src);
    if (!r) return false;
    return aura::compiler::types::is_bool(*r) &&
           aura::compiler::types::as_bool(*r);
}

// ── Test 1: BitVec with explicit width ───────────────────

bool test_bitvec_explicit_width() {
    std::println("\n--- Test 1: BitVec with explicit width ---");
    aura::compiler::CompilerService cs;
    CHECK(eval_is_true(cs,
        "(require \"std/eda\" all:)"
        "(eda:bitvec? (make-eda:bitvec 8))"),
        "BitVec predicate accepts explicit-width BitVec");
    CHECK(eval_is_true(cs,
        "(= (eda:bitvec-width (make-eda:bitvec 16)) 16)"),
        "BitVec-width returns the explicit width");
    return true;
}

// ── Test 2: BitVec with parametric width ─────────────────

bool test_bitvec_parametric_width() {
    std::println("\n--- Test 2: BitVec with parametric width ---");
    aura::compiler::CompilerService cs;
    // Parametric widths are stored as eda:bitvec-param lists
    // (separate tag from eda:bitvec). Aura's symbol? always
    // returns #f (symbols aren't first-class) and eq? returns
    // #f for symbols — we use eda:bitvec-param? to detect the
    // parametric case, and equal? on the cdr to compare names.
    CHECK(eval_is_true(cs,
        "(require \"std/eda\" all:)"
        "(eda:bitvec-param? (make-eda:bitvec-param 'data-width))"),
        "make-eda:bitvec-param produces an eda:bitvec-param value");
    CHECK(eval_is_true(cs,
        "(require \"std/eda\" all:)"
        "(equal? (eda:bitvec-width (make-eda:bitvec-param 'addr-width)) 'addr-width)"),
        "make-eda:bitvec-param stores the parameter name in cdr");
    return true;
}

// ── Test 3: BitVec compatibility — equal widths ───────────

bool test_bitvec_compat_equal() {
    std::println("\n--- Test 3: BitVec compatibility — equal widths ---");
    aura::compiler::CompilerService cs;
    CHECK(eval_is_true(cs,
        "(require \"std/eda\" all:)"
        "(eda:bitvec-compatible? (make-eda:bitvec 8) (make-eda:bitvec 8))"),
        "BitVec(8) compatible with BitVec(8)");
    CHECK(eval_is_true(cs,
        "(eda:bitvec-compatible? (make-eda:bitvec-32) (make-eda:bitvec-32))"),
        "make-eda:bitvec-32 compatible with itself");
    return true;
}

// ── Test 4: BitVec compatibility — mismatched widths ──────

bool test_bitvec_compat_mismatch() {
    std::println("\n--- Test 4: BitVec compatibility — mismatched widths ---");
    aura::compiler::CompilerService cs;
    CHECK(eval_is_true(cs,
        "(require \"std/eda\" all:)"
        "(not (eda:bitvec-compatible? (make-eda:bitvec 8) (make-eda:bitvec 16)))"),
        "BitVec(8) not compatible with BitVec(16)");
    CHECK(eval_is_true(cs,
        "(not (eda:bitvec-compatible? (make-eda:bitvec-8) (make-eda:bitvec-64)))"),
        "make-eda:bitvec-8 not compatible with make-eda:bitvec-64");
    return true;
}

// ── Test 5: BitVec compatibility — parametric defers ──────

bool test_bitvec_compat_parametric() {
    std::println("\n--- Test 5: BitVec compatibility — parametric defers ---");
    aura::compiler::CompilerService cs;
    // Note: eda:bitvec-compatible? returns the symbol 'parametric
    // when the resolution is deferred. We check the result with
    // equal? (since Aura's eq? returns #f for symbols).
    CHECK(eval_is_true(cs,
        "(require \"std/eda\" all:)"
        "(equal? (eda:bitvec-compatible? "
        "       (make-eda:bitvec-param 'w) "
        "       (make-eda:bitvec-param 'w)) 'parametric)"),
        "Parametric vs parametric returns 'parametric");
    CHECK(eval_is_true(cs,
        "(require \"std/eda\" all:)"
        "(equal? (eda:bitvec-compatible? "
        "       (make-eda:bitvec 8) "
        "       (make-eda:bitvec-param 'w)) 'parametric)"),
        "Concrete vs parametric returns 'parametric");
    return true;
}

// ── Test 6: ClockDomain compatibility ─────────────────────

bool test_clock_domain_compat() {
    std::println("\n--- Test 6: ClockDomain compatibility ---");
    aura::compiler::CompilerService cs;
    CHECK(eval_is_true(cs,
        "(require \"std/eda\" all:)"
        "(eda:clock-domain-compatible? "
        "  (make-eda:clock-domain 'main) "
        "  (make-eda:clock-domain 'main))"),
        "Same clock domain name → compatible");
    CHECK(eval_is_true(cs,
        "(not (eda:clock-domain-compatible? "
        "  (make-eda:clock-domain 'fast) "
        "  (make-eda:clock-domain 'slow)))"),
        "Different clock domain names → not compatible");
    CHECK(eval_is_true(cs,
        "(eda:clock-domain? eda:default-clock-domain)"),
        "eda:default-clock-domain is a ClockDomain");
    return true;
}

// ── Test 7: ResetType compatibility ───────────────────────

bool test_reset_type_compat() {
    std::println("\n--- Test 7: ResetType compatibility ---");
    aura::compiler::CompilerService cs;
    CHECK(eval_is_true(cs,
        "(require \"std/eda\" all:)"
        "(eda:reset-type-compatible? "
        "  (make-eda:reset-type 'async) "
        "  (make-eda:reset-type 'async))"),
        "Same reset kind → compatible");
    CHECK(eval_is_true(cs,
        "(not (eda:reset-type-compatible? "
        "  (make-eda:reset-type 'async) "
        "  (make-eda:reset-type 'sync)))"),
        "async vs sync → not compatible");
    CHECK(eval_is_true(cs,
        "(not (eda:reset-type-compatible? "
        "  eda:default-reset-type "
        "  (make-eda:reset-type 'async)))"),
        "default (sync) vs async → not compatible");
    return true;
}

// ── Test 8: TypedSignal full compatibility ────────────────

bool test_typed_signal_full_compat() {
    std::println("\n--- Test 8: TypedSignal full compatibility ---");
    aura::compiler::CompilerService cs;
    CHECK(eval_is_true(cs,
        "(require \"std/eda\" all:)"
        "(eda:typed-signal-compatible? "
        "  (make-eda:typed-signal 'data (make-eda:bitvec 8) "
        "    (make-eda:clock-domain 'main) (make-eda:reset-type 'sync)) "
        "  (make-eda:typed-signal 'data (make-eda:bitvec 8) "
        "    (make-eda:clock-domain 'main) (make-eda:reset-type 'sync)))"),
        "Two fully-matching typed signals are compatible");
    CHECK(eval_is_true(cs,
        "(require \"std/eda\" all:)"
        "(not (eda:typed-signal-compatible? "
        "  (make-eda:typed-signal 'data (make-eda:bitvec 8) "
        "    (make-eda:clock-domain 'main) (make-eda:reset-type 'sync)) "
        "  (make-eda:typed-signal 'data (make-eda:bitvec 16) "
        "    (make-eda:clock-domain 'main) (make-eda:reset-type 'sync))))"),
        "Width mismatch → typed signals not compatible");
    return true;
}
// Note: Test 6 (clock-domain-compatible?) and Test 7 (reset-type-compatible?)
// internally use `equal?` on symbol names. The compatible? check is
// implemented that way in eda.aura because eq? on Aura symbols always
// returns #f (the eq? primitive compares pointer-equality, but
// eval-time symbol values don't preserve identity the way interned
// parser symbols do).

// ═════════════════════════════════════════════════════════════
// Main test runner
// ═════════════════════════════════════════════════════════════

int run_tests() {
    std::println("═══ Issue #228 (Hardware IR Dependent Type System — Phase 1) ═══\n");

    test_bitvec_explicit_width();
    test_bitvec_parametric_width();
    test_bitvec_compat_equal();
    test_bitvec_compat_mismatch();
    test_bitvec_compat_parametric();
    test_clock_domain_compat();
    test_reset_type_compat();
    test_typed_signal_full_compat();

    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
}  // namespace aura_issue_228_detail

int aura_issue_228_run() { return aura_issue_228_detail::run_tests(); }

