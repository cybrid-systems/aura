// @category: integration
// @reason: Issue #300 — Live-object defragmentation observability foundation
//
// Validates (arena:defrag-stats) returns a 5-tuple:
//   (compaction-count
//    defrag-attempted-count
//    fragmentation-bp
//    wasted-bytes
//    compact-estimate-bytes)
//
// All 5 values are integers. fragmentation-bp is in basis points
// (0-10000 = 0%-100%). defrag-attempted-count is always 0 in the
// foundation (the actual defrag path is a separate follow-up).
//
// Empty workspace: all metrics are 0; fragmentation-bp == 0 (no
// capacity → empty ratio is defined as 0, not NaN/UB).
//
// Note: AC #4 (defrag-attempted is 0 after (arena:compact)) now
// passes. The pre-existing double-free in ~FlatAST() when
// (arena:compact) is called on a fresh CS after set-code was fixed
// in this same commit by adding 7 missing members to the FlatAST
// ctor initializer list (marker_ / dirty_ / ppa_dirty_ / verify_dirty_
// / verification_dirty_ / macro_dirty_ / narrowing_log_). Those
// columns were default-constructing with new_delete_resource, which
// produced a confused "double-free" ASAN report under compact that
// the sanitizer could not localize.
#include <iostream>
#include <string>
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace test_300_detail {

// Helper: extract 5-tuple (e1 . (e2 . (e3 . (e4 . e5)))) into 5 ints.
// Walks the pair chain: at each level, the cdr is either the next
// pair (1..4) or the terminal int (5). The terminal is recognized by
// is_int(cdr), not by a sentinel value.
static bool extract_5tuple(aura::compiler::CompilerService& cs,
                           const aura::compiler::types::EvalValue& v,
                           int64_t& e1, int64_t& e2, int64_t& e3,
                           int64_t& e4, int64_t& e5) {
    if (!aura::compiler::types::is_pair(v)) return false;
    auto p1_idx = aura::compiler::types::as_pair_idx(v);
    auto& pairs = cs.evaluator().pairs();
    if (p1_idx >= pairs.size()) return false;
    auto& p1 = pairs[p1_idx];
    if (!aura::compiler::types::is_int(p1.car)) return false;
    e1 = aura::compiler::types::as_int(p1.car);
    if (!aura::compiler::types::is_pair(p1.cdr)) return false;
    auto p2_idx = aura::compiler::types::as_pair_idx(p1.cdr);
    if (p2_idx >= pairs.size()) return false;
    auto& p2 = pairs[p2_idx];
    if (!aura::compiler::types::is_int(p2.car)) return false;
    e2 = aura::compiler::types::as_int(p2.car);
    if (!aura::compiler::types::is_pair(p2.cdr)) return false;
    auto p3_idx = aura::compiler::types::as_pair_idx(p2.cdr);
    if (p3_idx >= pairs.size()) return false;
    auto& p3 = pairs[p3_idx];
    if (!aura::compiler::types::is_int(p3.car)) return false;
    e3 = aura::compiler::types::as_int(p3.car);
    if (!aura::compiler::types::is_pair(p3.cdr)) return false;
    auto p4_idx = aura::compiler::types::as_pair_idx(p3.cdr);
    if (p4_idx >= pairs.size()) return false;
    auto& p4 = pairs[p4_idx];
    if (!aura::compiler::types::is_int(p4.car)) return false;
    e4 = aura::compiler::types::as_int(p4.car);
    // Terminal: p4.cdr is the int e5 (dotted pair chain)
    if (!aura::compiler::types::is_int(p4.cdr)) return false;
    e5 = aura::compiler::types::as_int(p4.cdr);
    return true;
}

bool test_returns_5tuple() {
    std::cout << "\n--- AC #1: returns 5-tuple ---\n";
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(arena:defrag-stats)");
    if (!r) { ++g_failed; std::cerr << "eval returned null\n"; return false; }
    int64_t e1, e2, e3, e4, e5;
    bool ok = extract_5tuple(cs, *r, e1, e2, e3, e4, e5);
    CHECK(ok, "result is a 5-tuple");
    return true;
}

bool test_empty_workspace_zero() {
    // AC #2: empty workspace \u2192 counters (only incremented by user
    // actions) are 0. fragmentation-bp and compact-estimate may be
    // non-zero because the main arena has its 8MB initial buffer
    // reserved \u2014 that's infrastructure state, not user state.
    std::cout << "\n--- AC #2: empty workspace \u2192 0 counters ---\n";
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(arena:defrag-stats)");
    if (!r) { ++g_failed; return false; }
    int64_t e1, e2, e3, e4, e5;
    if (!extract_5tuple(cs, *r, e1, e2, e3, e4, e5)) {
        ++g_failed; std::cerr << "not a 5-tuple\n"; return false;
    }
    CHECK(e1 == 0, "compaction-count == 0 (got " + std::to_string(e1) + ")");
    CHECK(e2 == 0, "defrag-attempted-count == 0 (got " + std::to_string(e2) + ")");
    CHECK(e4 == 0, "wasted-bytes == 0 (got " + std::to_string(e4) + ")");
    CHECK(e3 >= 0 && e3 <= 10000,
          "fragmentation-bp in [0, 10000] (got " + std::to_string(e3) + ")");
    return true;
}

bool test_5tuple_shape_via_aura() {
    std::cout << "\n--- AC #3: 5-tuple shape via Aura ---\n";
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define q 100)\")");
    // 5-tuple: (e1 . (e2 . (e3 . (e4 . e5))))
    // - 4 nested pairs (cadr/caddr/cadddr all pairs)
    // - terminal cdr is an int
    // - all 4 car slots are ints
    auto r = cs.eval("(let ((t (arena:defrag-stats)))"
                     " (and (pair? t)"
                     "       (pair? (cdr t))"
                     "       (pair? (cdr (cdr t)))"
                     "       (pair? (cdr (cdr (cdr t))))"
                     "       (integer? (cdr (cdr (cdr (cdr t)))))"
                     "       (integer? (car t))"
                     "       (integer? (car (cdr t)))"
                     "       (integer? (car (cdr (cdr t))))"
                     "       (integer? (car (cdr (cdr (cdr t)))))))");
    if (!r) { ++g_failed; return false; }
    auto& v = *r;
    bool is_t = aura::compiler::types::is_bool(v) && aura::compiler::types::as_bool(v);
    CHECK(is_t, "5-tuple has correct shape (4 pairs + terminal int, all int cars)");
    return true;
}

bool test_5tuple_stable_across_calls() {
    // AC #4: (arena:defrag) actually increments the defrag counter
    // (foundation B). Pre-existing dtor fix landed in commit 5d97fb40.
    std::cout << "\n--- AC #4: (arena:defrag) increments defrag-attempted-count ---\n";
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define a 1) (define b 2)\")");
    // Capture pre-defrag snapshot
    auto r0 = cs.eval("(arena:defrag-stats)");
    if (!r0) { ++g_failed; return false; }
    int64_t e1, e2, e3, e4, e5;
    if (!extract_5tuple(cs, *r0, e1, e2, e3, e4, e5)) {
        ++g_failed; return false; }
    int64_t defrag_before = e2;
    int64_t compact_est_before = e5;
    // Call defrag (B foundation: trims the unused tail, increments
    // stats_.defrag_attempted_count + last_defrag_saved).
    cs.eval("(arena:defrag)");
    auto r = cs.eval("(arena:defrag-stats)");
    if (!r) { ++g_failed; return false; }
    if (!extract_5tuple(cs, *r, e1, e2, e3, e4, e5)) {
        ++g_failed; return false; }
    CHECK(e2 == defrag_before + 1,
          "defrag-attempted-count incremented by 1 (was " + std::to_string(defrag_before)
          + ", now " + std::to_string(e2) + ")");
    CHECK(e5 <= compact_est_before,
          "compact-estimate did not grow (was " + std::to_string(compact_est_before)
          + ", now " + std::to_string(e5) + ")");
    CHECK(e3 >= 0 && e3 <= 10000,
          "fragmentation-bp in [0, 10000] (got " + std::to_string(e3) + ")");
    return true;
}

int run_tests() {
    std::cout << "═══ Issue #300 ═══\n";
    test_returns_5tuple();
    std::cout.flush();
    test_empty_workspace_zero();
    std::cout.flush();
    test_5tuple_shape_via_aura();
    std::cout.flush();
    test_5tuple_stable_across_calls();
    std::cout.flush();
    std::cout.flush();
    std::cout << "\n═══ Results: " << g_passed << "/" << g_passed + g_failed
              << " passed, " << g_failed << "/" << g_passed + g_failed
              << " failed ═══\n";
    return g_failed > 0 ? 1 : 0;
}

}

int aura_issue_300_run() { return test_300_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_300_run(); }
#endif
