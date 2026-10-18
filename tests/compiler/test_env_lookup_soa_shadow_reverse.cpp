// @category: unit
// @reason: Issue #4353 — Env::lookup's SoA walk scanned frame string
//          bindings FORWARD, so a TCO call frame holding copied ancestor
//          bindings (materialize of a cap frame that snapshotted outer
//          scopes) followed by fresh local binds resolved a shadowed name
//          to the STALE copy. Named-let bodies read the global instead of
//          the let binding (#4352 the same face with top-level named let).
//
//   AC1: named-let body reads let-shadowed global (11, not 7)
//   AC2: source-cite — SoA walk string scan matches in reverse (rbegin)
//   AC3 (#4595): after a top-level named let, a closure from a later
//          top-level let called through another procedure reads the
//          let binding (42), not the stale global copy (0)
//   AC4 (#4595): let* variant of the same face
//   AC5 (#4595): set! through the closure path writes the same cell
//          the read resolves (no split resolution)
//   AC6 (#4595): source-cite — letrec register-and-repoint gated on
//          !parent_is_live_top; cell lookups reverse-scan live top_
//   (param-shadow + named-let recursion CLI faces live in
//    tests/python/run-tests.sh 4353-named-let-shadow.)

#include "test_harness.hpp"

#include <print>
#include <fstream>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

// AC1: outer let binds gv=11 shadowing global gv=7; the named-let loop
// body must resolve gv through the SoA parent chain to the LET binding.
static void ac1_named_let_shadow_global() {
    std::println("\n--- #4353 AC1: named-let reads let-shadowed global ---");
    CompilerService cs;
    auto r = cs.eval("(begin (define gv 7)"
                     " (define (h3 x)"
                     " (let ((gv (+ x 1)))"
                     " (let lp ((i 0)) (if (>= i 2) gv (lp (+ i 1))))))"
                     " (h3 10))");
    if (!r.has_value())
        std::println("AC1 diagnostic: {}", r.error().message);
    CHECK(r.has_value(), "AC1: eval ok");
    if (r.has_value())
        CHECK(is_int(*r) && as_int(*r) == 11,
              "AC1: body reads let gv=11 (pre-fix: stale global copy 7)");
}

// AC4: source-cite — the SoA walk's string scan matches in reverse.
static void ac2_source_cite_reverse_scan() {
    std::println("\n--- #4353 AC4: source-cite reverse scan ---");
    for (const auto& p : {std::string("src/compiler/evaluator_env.cpp"),
                          std::string("../src/compiler/evaluator_env.cpp"),
                          std::string("../../src/compiler/evaluator_env.cpp")}) {
        std::ifstream in(p);
        if (!in)
            continue;
        std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(src.find("pfr.bindings_.rbegin()") != std::string::npos,
              "AC4: SoA walk string scan uses rbegin (reverse match)");
        return;
    }
    CHECK(false, "AC4: evaluator_env.cpp not found from CWD");
}

// AC3 (#4595): a top-level named let must not freeze top_ behind a
// self-copy frame — a closure created in a LATER top-level let and
// called through another procedure reads the let binding, not the
// global cell that the named-let-time copy still holds.
static void ac3_4595_top_named_let_shadow() {
    std::println("\n--- #4595 AC3: top-level named let + later let-shadow ---");
    CompilerService cs;
    auto r = cs.eval("(begin (define x 0)"
                     " (define (call th) (th))"
                     " (let loop ((r 0)) (if (< r 0) (loop (+ r 1)) #t))"
                     " (let ((x 42)) (call (lambda () x))))");
    if (!r.has_value())
        std::println("AC3 diagnostic: {}", r.error().message);
    CHECK(r.has_value(), "AC3: eval ok");
    if (r.has_value())
        CHECK(is_int(*r) && as_int(*r) == 42,
              "AC3: closure reads let x=42 (pre-fix: stale global copy 0)");
}

// AC4 (#4595): let* variant — the second binding in a let* created
// after the top-level named let shadows the same-named global.
static void ac4_4595_letstar_variant() {
    std::println("\n--- #4595 AC4: let* variant ---");
    CompilerService cs;
    auto r = cs.eval("(begin (define x 0)"
                     " (define (call th) (th))"
                     " (let loop ((r 0)) (if (< r 0) (loop (+ r 1)) #t))"
                     " (let* ((f 1) (x 42)) (call (lambda () x))))");
    if (!r.has_value())
        std::println("AC4 diagnostic: {}", r.error().message);
    CHECK(r.has_value(), "AC4: eval ok");
    if (r.has_value())
        CHECK(is_int(*r) && as_int(*r) == 42,
              "AC4: let* closure reads x=42 (pre-fix: stale global copy 0)");
}

// AC5 (#4595): set! through the closure path writes the cell the read
// resolves — pre-fix the set! walker (lookup_cell_ptr/index forward
// scans) wrote the OLDEST binding while the read returned the newest.
static void ac5_4595_setbang_consistency() {
    std::println("\n--- #4595 AC5: set! and read agree through closure ---");
    CompilerService cs;
    auto r = cs.eval("(begin (define x 0)"
                     " (define (call th) (th))"
                     " (let loop ((r 0)) (if (< r 0) (loop (+ r 1)) #t))"
                     " (let ((x 7)) (call (lambda () (begin (set! x 42) x)))))");
    if (!r.has_value())
        std::println("AC5 diagnostic: {}", r.error().message);
    CHECK(r.has_value(), "AC5: eval ok");
    if (r.has_value())
        CHECK(is_int(*r) && as_int(*r) == 42,
              "AC5: thunk returns 42 after set! (split-resolution guard)");
}

// AC6 (#4595): source-cite — the letrec arm's register-and-repoint is
// gated on !parent_is_live_top (top_ never carries a frozen self-copy
// frame), and the cell lookups reverse-scan live top_.
static void ac6_4595_source_cite_gates() {
    std::println("\n--- #4595 AC6: source-cite lookup-side fix gates ---");
    bool found_env_cpp = false;
    for (const auto& p : {std::string("src/compiler/evaluator_env.cpp"),
                          std::string("../src/compiler/evaluator_env.cpp"),
                          std::string("../../src/compiler/evaluator_env.cpp")}) {
        std::ifstream in(p);
        if (!in)
            continue;
        found_env_cpp = true;
        std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(src.find("aura::serve::g_current_fiber == nullptr && bindings_.empty()") !=
                  std::string::npos,
              "AC6: empty-capture live-top consult fiber-gated (host-thread only)");
        CHECK(src.find("cap_snap_->bindings_.empty() && cap_snap_->bindings_symid_.empty()") !=
                  std::string::npos,
              "AC6: consult only for truly-empty captures (symid-only keeps capture order)");
        CHECK(src.find("auto tb4595 = owner_->top_env().bindings();") != std::string::npos,
              "AC6: cell-lookup empty-capture consults use tb4595");
        break;
    }
    CHECK(found_env_cpp, "AC6: evaluator_env.cpp found from CWD");
    bool found_flat_cpp = false;
    for (const auto& p : {std::string("src/compiler/evaluator_eval_flat.cpp"),
                          std::string("../src/compiler/evaluator_eval_flat.cpp"),
                          std::string("../../src/compiler/evaluator_eval_flat.cpp")}) {
        std::ifstream in(p);
        if (!in)
            continue;
        found_flat_cpp = true;
        std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(src.find("fixed lookup-side in Env::lookup /") != std::string::npos,
              "AC6: letrec registration kept, fix documented lookup-side");
        break;
    }
    CHECK(found_flat_cpp, "AC6: evaluator_eval_flat.cpp found from CWD");
}

} // namespace

int run_test_env_lookup_soa_shadow_reverse() {
    ac1_named_let_shadow_global();
    ac2_source_cite_reverse_scan();
    ac3_4595_top_named_let_shadow();
    ac4_4595_letstar_variant();
    ac5_4595_setbang_consistency();
    ac6_4595_source_cite_gates();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_env_lookup_soa_shadow_reverse();
}
#endif
