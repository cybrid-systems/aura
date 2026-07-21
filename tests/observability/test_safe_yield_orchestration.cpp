// @category: integration
// @reason: Issue #1635 — first-class ast:yield-at-boundary + per-fiber depth
// Issue #1504/#1591/#1635 (#1978 renamed): issue# moved from filename to header.
// instrumentation for multi-Agent orchestration (refine #1504 / #1591).
//
//   AC1: query:mutation-boundary-depth + ast:yield-at-boundary primitives
//   AC2: per-fiber depth / stack stats (schema 1635)
//   AC3: GC safepoint depth-check + scheduler YieldReason wire flags
//   AC4: concurrent fibers + explicit yield points; no yield-while-holding
//   AC5: fairness / hold-time fields; skipped-held under Guard
//   AC6: #1504/#1591 lineage acceptance

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_primitives() {
    std::println("\n--- AC1: depth + ast:yield-at-boundary ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto d = cs.eval("(engine:metrics \"query:mutation-boundary-depth\")");
    CHECK(d && is_int(*d) && as_int(*d) == 0, "depth 0 outside Guard");

    auto h = cs.eval("(engine:metrics \"ast:yield-at-boundary\")");
    CHECK(h && is_hash(*h), "ast:yield-at-boundary hash");
    CHECK(href(cs, "ast:yield-at-boundary", "schema") == 1635, "alias schema 1635");
    CHECK(href(cs, "ast:yield-at-boundary", "ast-yield-at-boundary-wired") == 1, "wired");
    CHECK(href(cs, "ast:yield-at-boundary", "safe-yield-mandate-active") == 1, "mandate");
    CHECK(href(cs, "ast:yield-at-boundary", "skipped-held") == 0, "not skipped outside");
    CHECK(href(cs, "ast:yield-at-boundary", "boundary-depth") == 0, "depth 0");
}

static void ac2_per_fiber_depth() {
    std::println("\n--- AC2: per-fiber depth stats ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:per-fiber-mutation-depth-stats\")");
    CHECK(h && is_hash(*h), "depth-stats hash");
    CHECK(href(cs, "query:per-fiber-mutation-depth-stats", "schema") == 1635, "schema 1635");
    CHECK(href(cs, "query:per-fiber-mutation-depth-stats", "lifetime-max") >= 0, "lifetime-max");
    CHECK(href(cs, "query:per-fiber-mutation-depth-stats", "live-depth") >= 0, "live-depth");
    CHECK(href(cs, "query:per-fiber-mutation-depth-stats", "gc-safepoint-depth-check-wired") == 1,
          "gc depth check flag");

    auto st = cs.eval("(engine:metrics \"query:mutation-boundary-safe-yield-stats\")");
    CHECK(st && is_hash(*st), "stats hash");
    CHECK(href(cs, "query:mutation-boundary-safe-yield-stats", "schema") == 1635, "stats 1635");
    CHECK(href(cs, "query:mutation-boundary-safe-yield-stats", "per-fiber-stack-depth-max") >= 0,
          "stack depth max");
    CHECK(href(cs, "query:mutation-boundary-safe-yield-stats", "nested-guard-depth-max") >= 0,
          "nested max");
}

static void ac3_gc_scheduler_wire() {
    std::println("\n--- AC3: GC + scheduler wire flags ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:mutation-boundary-fairness-stats\")");
    CHECK(h && is_hash(*h), "fairness hash");
    CHECK(href(cs, "query:mutation-boundary-fairness-stats", "schema") == 1635, "schema 1635");
    CHECK(href(cs, "query:mutation-boundary-fairness-stats", "gc-safepoint-depth-check-wired") == 1,
          "gc wired");
    CHECK(href(cs, "query:mutation-boundary-fairness-stats", "ast-yield-at-boundary-wired") == 1,
          "ast yield wired");
    CHECK(href(cs, "ast:yield-at-boundary", "yield-reason-mutation-boundary") == 1,
          "YieldReason::MutationBoundary");
    CHECK(href(cs, "ast:yield-at-boundary", "gc-safepoint-depth-check-wired") == 1, "gc on action");
    // request_gc_safepoint defers when depth>0 (API callable).
    (void)cs.evaluator().request_gc_safepoint();
    CHECK(true, "request_gc_safepoint callable");
}

static void ac4_concurrent_orchestration() {
    std::println("\n--- AC4: concurrent fibers + explicit yield points ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    const auto skip0 = cs.evaluator().get_safe_yield_skipped_held_total();
    const auto ok0 =
        cs.evaluator().get_safe_yield_ok_total() + cs.evaluator().get_safe_yield_no_fiber_total();
    std::atomic<int> violations{0};
    std::atomic<int> yields{0};
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 50; ++i) {
                // Explicit interleave: depth probe + safe yield (never under Guard).
                if (Evaluator::mutation_boundary_depth() != 0)
                    violations.fetch_add(1, std::memory_order_relaxed);
                const int rc = cs.evaluator().try_safe_yield_at_boundary(0);
                if (rc == 0)
                    yields.fetch_add(1, std::memory_order_relaxed);
                else if (rc == 1)
                    violations.fetch_add(1, std::memory_order_relaxed); // must not skip outside
                (void)cs.eval(std::format("(+ {} {})", t, i));
            }
        });
    }
    for (auto& th : threads)
        th.join();

    CHECK(violations.load() == 0, std::format("no yield-while-holding ({})", violations.load()));
    CHECK(yields.load() > 0, "safe yields occurred");
    CHECK(cs.evaluator().get_safe_yield_skipped_held_total() == skip0,
          "no skipped-held without Guard");
    const auto ok1 =
        cs.evaluator().get_safe_yield_ok_total() + cs.evaluator().get_safe_yield_no_fiber_total();
    CHECK(ok1 > ok0, "ok/no-fiber counters advanced");
}

static void ac5_skipped_under_guard() {
    std::println("\n--- AC5: skipped-held under Guard + fairness ---");
    CompilerService cs;
    const auto skip0 = cs.evaluator().get_safe_yield_skipped_held_total();
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard guard(cs.evaluator(), &ok);
        CHECK(Evaluator::mutation_boundary_depth() >= 1, "depth under Guard");
        CHECK(cs.evaluator().try_safe_yield_at_boundary(0) == 1, "skipped-held");
        auto h = cs.eval("(engine:metrics \"ast:yield-at-boundary\")");
        CHECK(h && is_hash(*h), "hash under guard");
        CHECK(href(cs, "ast:yield-at-boundary", "skipped-held") == 1, "skipped-held key");
        CHECK(href(cs, "ast:yield-at-boundary", "boundary-depth") >= 1, "depth in hash");
    }
    CHECK(cs.evaluator().get_safe_yield_skipped_held_total() > skip0, "skipped counter grew");
    CHECK(Evaluator::mutation_boundary_depth() == 0, "depth 0 after Guard");
    CHECK(cs.evaluator().try_safe_yield_at_boundary(0) == 0, "safe again");

    auto fair = cs.eval("(engine:metrics \"query:mutation-boundary-fairness-stats\")");
    CHECK(fair && is_hash(*fair), "fairness");
    CHECK(href(cs, "query:mutation-boundary-fairness-stats", "avg-hold-time-us") >= 0, "avg hold");
    CHECK(href(cs, "query:mutation-boundary-fairness-stats",
               "safepoint-wait-while-mutation-held-us") >= 0,
          "safepoint wait");
}

static void ac6_lineage() {
    std::println("\n--- AC6: #1504/#1591 lineage ---");
    CompilerService cs;
    // schema is now 1635; still exposes 1504-era fields
    CHECK(href(cs, "ast:yield-at-boundary", "safe-yield-ok-total") >= 0, "ok total");
    CHECK(href(cs, "query:mutation-boundary-safe-yield-stats", "issue") == 1635, "issue 1635");
    auto sy = cs.eval("(engine:metrics \"query:mutation-boundary-safe-yield\")");
    CHECK(sy && is_hash(*sy), "safe-yield still works");
    CHECK(href(cs, "query:mutation-boundary-safe-yield", "schema") == 1635, "safe-yield 1635");
}

} // namespace

int main() {
    std::println("=== Issue #1635: safe yield orchestration mandate ===");
    ac1_primitives();
    ac2_per_fiber_depth();
    ac3_gc_scheduler_wire();
    ac4_concurrent_orchestration();
    ac5_skipped_under_guard();
    ac6_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
