// @category: integration
// @reason: uses CompilerService to verify EDSL readiness aggregator

// test_issue_440_edsl_readiness.cpp — Issue #440:
// Consolidated Task 1 Review: Workspace + Query/Mutate
// Primitives - AI Multi-Round Reliability & Enhancement
// Roadmap.
//
// #440 is a synthesis issue that summarizes the EDSL
// production readiness state. Most of its enumerated
// gaps already have separate follow-up issues (#420,
// #421, #423, #424, #425, #438, #439) — the scope-limited
// close ships the OBSERVABILITY + TEST layer:
//
//   (stats:get "query:edsl-readiness") — new 6-field hash that
//   aggregates the top EDSL production-readiness
//   signals in one call. Replaces the conservative
//   "ask 30+ separate (query:*-stats) primitives" pattern
//   with a single observable. The 6 fields:
//     - closure-stale-refresh     (#531)
//     - linear-check-pass         (#149)
//     - atomic-batch-commits      (#241)
//     - stable-ref-invalidations  (#417)
//     - occurrence-stale-refreshes (#609)
//     - dirty-block-rate          (#429 live SoA dirty state)
//
// Test cases:
//   AC1:  fresh Evaluator — all 6 fields are non-negative
//   AC2:  one define + eval — closure-stale-refresh >= 0,
//         dirty-block-rate >= 0
//   AC3:  hash 6 fields present + each is non-negative
//   AC4:  empty workspace — no crash
//   AC5:  repeated (stats:get "query:edsl-readiness") calls return
//         consistent hashes (idempotent)
//   AC6:  stats:list includes the new primitive
//   AC7:  stats:count >= 43 (was 42 in #431, now 43 in #440)

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_440_detail {
static int g_passed = 0;
static int g_failed = 0;

static aura::compiler::types::EvalValue run_on(aura::compiler::CompilerService& cs,
                                               std::string_view src) {
    auto r = cs.eval(src);
    if (!r) {
        std::println(std::cerr, "    [eval error: {}]", r.error().format());
        return aura::compiler::types::make_void();
    }
    return *r;
}

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"query:edsl-readiness\") '{}')", key));
    if (!r)
        return -1;
    if (!aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cout, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

// ═══════════════════════════════════════════════════════════
// AC1: fresh Evaluator — all 6 fields non-negative
// ═══════════════════════════════════════════════════════════
bool test_fresh_evaluator() {
    std::println("\n--- AC1: fresh Evaluator ---");
    aura::compiler::CompilerService cs;
    static const char* kFields[] = {
        "closure-stale-refresh",    "linear-check-pass",          "atomic-batch-commits",
        "stable-ref-invalidations", "occurrence-stale-refreshes", "dirty-block-rate",
    };
    bool all_ok = true;
    for (auto* k : kFields) {
        auto v = hash_int(cs, k);
        if (v < 0) {
            std::println("    [field {} returned {}]", k, v);
            all_ok = false;
        }
    }
    CHECK(all_ok, "all 6 fields present and non-negative on fresh Evaluator");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC2: one define + eval — basic counters stay sane
// ═══════════════════════════════════════════════════════════
bool test_one_define_eval() {
    std::println("\n--- AC2: one define + eval ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    run_on(cs, "(display (f 41))");
    auto csf = hash_int(cs, "closure-stale-refresh");
    auto dbr = hash_int(cs, "dirty-block-rate");
    CHECK(csf >= 0, "after one define + eval: closure-stale-refresh >= 0");
    CHECK(dbr >= 0 && dbr <= 100, "after one define + eval: dirty-block-rate in 0..100");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC3: 6 fields present + each is a non-negative integer
// ═══════════════════════════════════════════════════════════
bool test_six_fields_present() {
    std::println("\n--- AC3: 6 fields present + non-negative ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(set-code \"(define (f x) (+ x 1))\")");
    run_on(cs, "(eval-current)");
    static const char* kFields[] = {
        "closure-stale-refresh",    "linear-check-pass",          "atomic-batch-commits",
        "stable-ref-invalidations", "occurrence-stale-refreshes", "dirty-block-rate",
    };
    bool all_ok = true;
    for (auto* k : kFields) {
        auto v = hash_int(cs, k);
        if (v < 0) {
            std::println("    [field {} returned {}]", k, v);
            all_ok = false;
        }
    }
    CHECK(all_ok, "all 6 fields present and non-negative after eval");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC4: empty workspace — no crash
// ═══════════════════════════════════════════════════════════
bool test_empty_workspace_no_crash() {
    std::println("\n--- AC4: empty workspace — no crash ---");
    aura::compiler::CompilerService cs;
    auto csf = hash_int(cs, "closure-stale-refresh");
    CHECK(csf >= 0, "empty workspace: closure-stale-refresh >= 0");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC5: idempotence — repeated calls consistent
// ═══════════════════════════════════════════════════════════
bool test_idempotent_observable() {
    std::println("\n--- AC5: idempotence ---");
    aura::compiler::CompilerService cs;
    auto a = hash_int(cs, "closure-stale-refresh");
    auto b = hash_int(cs, "closure-stale-refresh");
    CHECK(a == b, "two consecutive calls return the same closure-stale-refresh");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC6: stats:list includes the new primitive
// ═══════════════════════════════════════════════════════════
bool test_stats_list_includes() {
    std::println("\n--- AC6: stats:list includes the new primitive ---");
    aura::compiler::CompilerService cs;
    auto r = run_on(
        cs, "(letrec ((find? (lambda (needle hay) "
            "                (if (pair? hay) "
            "                    (if (string=? (car hay) needle) #t (find? needle (cdr hay))) "
            "                    #f)))) "
            "  (if (find? \"query:edsl-readiness\" (stats:list)) 1 0))");
    bool included = aura::compiler::types::is_int(r) && aura::compiler::types::as_int(r) == 1;
    CHECK(included, "stats:list includes query:edsl-readiness");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC7: stats:count >= 43
// ═══════════════════════════════════════════════════════════
bool test_stats_count() {
    std::println("\n--- AC7: stats:count is up to date ---");
    aura::compiler::CompilerService cs;
    auto r = run_on(cs, "(stats:count)");
    bool ok = aura::compiler::types::is_int(r) && aura::compiler::types::as_int(r) >= 43;
    CHECK(ok, "stats:count >= 43 (was 42 in #431, now 43 in #440)");
    if (aura::compiler::types::is_int(r)) {
        std::println("    [stats:count = {}]", aura::compiler::types::as_int(r));
    }
    return true;
}

} // namespace aura_issue_440_detail

int aura_issue_440_edsl_readiness_run() {
    using namespace aura_issue_440_detail;
    std::println("═══ Issue #440 EDSL production-readiness aggregator tests ═══");

    test_fresh_evaluator();
    test_one_define_eval();
    test_six_fields_present();
    test_empty_workspace_no_crash();
    test_idempotent_observable();
    test_stats_list_includes();
    test_stats_count();

    std::println("\n──────────────────────────────────────");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_440_edsl_readiness_run();
}
#endif
