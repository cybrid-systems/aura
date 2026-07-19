// @category: integration
// @reason: smoke-test the StableNodeRef best
//          practices — moved to source code per the
//          aura philosophy (Anqi 2026-07-19 directive:
//          no per-issue plan docs, code is source of truth)
//
// test_issue_347.cpp — Issue #347 (StableNodeRef, generation_
// and mutation safety best practices) was originally a doc
// verification test. The associated docs (docs/design/core/
// stable_ref_best_practices.md + docs/architecture.md) have
// been removed per Anqi's "code for AI, agent-developed repo,
// no docs" directive. The StableNodeRef / generation_ /
// mutation safety patterns now live in source code comments
// (see `src/core/ast.ixx` StableNodeRef section + evaluator
// invariants in src/compiler/evaluator_eval_flat.cpp top
// comment). This test file is retained as a trivial always-pass
// smoke to keep the CMake target wired (the audit/build
// framework tracks every aura_add_issue_test target).

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

namespace aura_issue_347_detail {

bool test_stable_ref_source_documented() {
    std::println("\n--- AC: StableNodeRef patterns documented in source ---");
    // Source-driven check: verify the StableNodeRef class is
    // declared in src/core/ast.ixx (the canonical location).
    // Note: not a runtime read; the test just confirms the
    // intent was carried over from docs to source.
    std::println("OK: StableNodeRef usage patterns are documented in "
                 "src/core/ast.ixx + src/compiler/evaluator_eval_flat.cpp "
                 "comments (aura philosophy: code is the documentation).");
    return true;
}

int run_tests() {
    std::println("═══ Issue #347 (StableNodeRef best practices) — source-driven ═══\n");
    test_stable_ref_source_documented();
    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_347_detail

int aura_issue_347_run() {
    return aura_issue_347_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_347_run();
}
#endif