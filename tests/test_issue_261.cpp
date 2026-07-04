// @category: integration
// @reason: uses CompilerService ast:recycle/compact/snapshot lifecycle APIs


#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_261_detail {

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

bool test_recycle_primitive() {
    std::println("\n--- AC1: ast:recycle-nodes ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define x 1)\")")) {
        ++g_failed;
        return false;
    }
    CHECK(run_int(cs, "(ast:recycle-nodes)") >= 0, "ast:recycle-nodes returns non-negative int");
    return true;
}

bool test_compact_primitive() {
    std::println("\n--- AC2: ast:compact-nodes ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(begin (define x 1) (define y 2))\")")) {
        ++g_failed;
        return false;
    }
    CHECK(run_int(cs, "(ast:compact-nodes)") >= 0, "ast:compact-nodes returns non-negative int");
    return true;
}

bool test_snapshot_restore_hook() {
    std::println("\n--- AC3: snapshot/restore recycle hooks ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define x 42)\")")) {
        ++g_failed;
        return false;
    }
    auto snap_id = run_int(cs, "(ast:snapshot \"s261\")");
    CHECK(snap_id >= 0, "snapshot created");
    std::string restore_src = "(ast:restore " + std::to_string(snap_id) + ")";
    CHECK(run_bool(cs, restore_src), "restore succeeds");
    return true;
}

int run_tests() {
    std::println("Issue #261 (FlatAST NodeId lifecycle)\n");
    test_recycle_primitive();
    test_compact_primitive();
    test_snapshot_restore_hook();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_261_detail

int aura_issue_261_run() {
    return aura_issue_261_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_261_run();
}
#endif