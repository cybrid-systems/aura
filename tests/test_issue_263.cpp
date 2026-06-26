// @category: integration
// @reason: uses CompilerService snapshot/restore + post-restore validation


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_263_detail {

static int64_t run_int(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool run_bool(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    return r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r);
}

bool test_validate_post_restore_primitive() {
    std::println("\n--- AC1: (ast:validate-post-restore) primitive ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define x 42)\")")) {
        ++g_failed;
        return false;
    }
    auto v = run_int(cs, "(hash-ref (ast:validate-post-restore) \"violations\")");
    CHECK(v == 0, "(ast:validate-post-restore) violations == 0 on fresh workspace");
    return true;
}

bool test_snapshot_restore_post_restore_stats() {
    std::println("\n--- AC2: ast:restore runs post-restore validation ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define x 1)\")")) {
        ++g_failed;
        return false;
    }
    auto snap = run_int(cs, "(ast:snapshot \"s263\")");
    CHECK(snap >= 0, "snapshot created");
    (void)cs.eval("(set-code \"(define x 99)\")");
    std::string restore = "(ast:restore " + std::to_string(snap) + ")";
    CHECK(run_bool(cs, restore), "restore succeeds");
    auto violations = run_int(cs, "(hash-ref (ast:post-restore-stats) \"violations\")");
    CHECK(violations == 0, "post-restore-stats violations == 0 after restore");
    return true;
}

bool test_restore_preserves_eval_result() {
    std::println("\n--- AC3: restore preserves snapshotted workspace ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define x 42)\")")) {
        ++g_failed;
        return false;
    }
    auto snap = run_int(cs, "(ast:snapshot \"s263-eval\")");
    if (snap < 0) {
        ++g_failed;
        return false;
    }
    (void)cs.eval("(set-code \"(define x 0)\")");
    std::string restore = "(ast:restore " + std::to_string(snap) + ")";
    CHECK(run_bool(cs, restore), "restore succeeds");
    auto x = run_int(cs, "(eval-current)");
    CHECK(x == 42, "eval-current returns 42 after restore");
    return true;
}

int run_tests() {
    std::println("Issue #263 (workspace snapshot/restore consistency)\n");
    test_validate_post_restore_primitive();
    test_snapshot_restore_post_restore_stats();
    test_restore_preserves_eval_result();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}

int aura_issue_263_run() { return aura_issue_263_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_263_run(); }
#endif