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

} // namespace

int run_test_env_lookup_soa_shadow_reverse() {
    ac1_named_let_shadow_global();
    ac2_source_cite_reverse_scan();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_env_lookup_soa_shadow_reverse();
}
#endif
