// @category: integration
// @reason: Issue #298 — incremental compilation effectiveness metrics
//
// Validates (query:incremental-effectiveness) returns a 4-tuple:
//   (recompile-ratio cascade-depth bridge-overhead fallback-freq)
//
// All 4 values are integers. Ratio is in basis points (0-10000).
// Empty workspace: all metrics are 0.
#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace test_298_detail {

// Helper: extract 4-tuple (e1 . (e2 . (e3 . e4))) into 4 ints.
// Uses the pairs() accessor + pair-by-pair traversal.
static bool extract_4tuple(aura::compiler::CompilerService& cs,
                           const aura::compiler::types::EvalValue& v, int64_t& e1, int64_t& e2,
                           int64_t& e3, int64_t& e4) {
    if (!aura::compiler::types::is_pair(v)) {
        std::println(std::cerr, "D: not pair");
        return false;
    }
    auto p1_idx = aura::compiler::types::as_pair_idx(v);
    auto& pairs = cs.evaluator().pairs();
    if (p1_idx >= pairs.size()) {
        std::println(std::cerr, "D: p1 OOR");
        return false;
    }
    auto& p1 = pairs[p1_idx];
    if (!aura::compiler::types::is_int(p1.car))
        return false;
    e1 = aura::compiler::types::as_int(p1.car);
    if (!aura::compiler::types::is_pair(p1.cdr)) {
        std::println(std::cerr, "D: p1.cdr not pair");
        return false;
    }
    auto p2_idx = aura::compiler::types::as_pair_idx(p1.cdr);
    if (p2_idx >= pairs.size()) {
        std::println(std::cerr, "D: p2 OOR");
        return false;
    }
    auto& p2 = pairs[p2_idx];
    if (!aura::compiler::types::is_int(p2.car)) {
        std::println(std::cerr, "D: p2.car not int");
        return false;
    }
    e2 = aura::compiler::types::as_int(p2.car);
    if (!aura::compiler::types::is_pair(p2.cdr))
        return false;
    auto p3_idx = aura::compiler::types::as_pair_idx(p2.cdr);
    if (p3_idx >= pairs.size())
        return false;
    auto& p3 = pairs[p3_idx];
    if (!aura::compiler::types::is_int(p3.car))
        return false;
    e3 = aura::compiler::types::as_int(p3.car);
    // Terminal: p3.cdr is the int e4 (dotted pair chain)
    if (!aura::compiler::types::is_int(p3.cdr))
        return false;
    e4 = aura::compiler::types::as_int(p3.cdr);
    return true;
}

bool test_returns_4tuple() {
    std::println("\n--- AC #1: returns 4-tuple ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(query:incremental-effectiveness)");
    if (!r) {
        ++g_failed;
        std::println(std::cerr, "eval returned null");
        return false;
    }
    int64_t e1, e2, e3, e4;
    bool ok = extract_4tuple(cs, *r, e1, e2, e3, e4);
    CHECK(ok, "result is a 4-tuple");
    return true;
}

bool test_empty_workspace_zero() {
    std::println("\n--- AC #2: empty workspace → 0 metrics ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(query:incremental-effectiveness)");
    if (!r) {
        ++g_failed;
        return false;
    }
    int64_t e1, e2, e3, e4;
    if (!extract_4tuple(cs, *r, e1, e2, e3, e4)) {
        ++g_failed;
        std::println(std::cerr, "not a 4-tuple");
        return false;
    }
    CHECK(e1 == 0, "recompile-ratio == 0 (got " + std::to_string(e1) + ")");
    CHECK(e2 == 0, "cascade-depth == 0 (got " + std::to_string(e2) + ")");
    CHECK(e3 == 0, "bridge-overhead == 0 (got " + std::to_string(e3) + ")");
    CHECK(e4 == 0, "fallback-freq == 0 (got " + std::to_string(e4) + ")");
    return true;
}

bool test_ratio_basis_points() {
    std::println("\n--- AC #3: recompile-ratio in basis points ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define x 1) (define y 2)\")");
    auto r = cs.eval("(query:incremental-effectiveness)");
    if (!r) {
        ++g_failed;
        return false;
    }
    int64_t e1, e2, e3, e4;
    if (!extract_4tuple(cs, *r, e1, e2, e3, e4)) {
        ++g_failed;
        return false;
    }
    CHECK(e1 >= 0 && e1 <= 10000, "ratio in [0, 10000] (got " + std::to_string(e1) + ")");
    return true;
}

bool test_4tuple_shape_via_aura() {
    std::println("\n--- AC #4: 4-tuple shape via Aura ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define z 100)\")");
    // Structure: (e1 . (e2 . (e3 . e4))) — a dotted pair
    // chain. The terminal cdr is an int, not a pair. So we
    // check (cdr (cdr (cdr t))) is an int (the terminal).
    auto r = cs.eval("(let ((t (query:incremental-effectiveness)))"
                     " (and (pair? t) (pair? (cdr t))"
                     "       (pair? (cdr (cdr t))) (integer? (cdr (cdr (cdr t))))"
                     "       (integer? (car t)) (integer? (car (cdr t)))"
                     "       (integer? (car (cdr (cdr t))))))");
    if (!r) {
        ++g_failed;
        return false;
    }
    auto& v = *r;
    bool is_t = aura::compiler::types::is_bool(v) && aura::compiler::types::as_bool(v);
    CHECK(is_t, "4-tuple has correct shape (4 pairs, all int cars)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #298 ═══");
    test_returns_4tuple();
    test_empty_workspace_zero();
    test_ratio_basis_points();
    test_4tuple_shape_via_aura();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}

} // namespace test_298_detail

int aura_issue_298_run() {
    return test_298_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_298_run();
}
#endif
