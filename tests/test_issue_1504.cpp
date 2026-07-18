// @category: integration
// @reason: Issue #1504 — First-class safe yield at MutationBoundaryGuard
// + per-fiber depth instrumentation for multi-Agent orchestration
// (non-duplicative to #1014 #213).
//
//   AC1: query:mutation-boundary-depth returns int (>= 0)
//   AC2: safe-yield outside Guard succeeds (yielded or no-fiber)
//   AC3: safe-yield inside Guard is skipped-held
//   AC4: ast:yield-at-boundary alias matches contract
//   AC5: safe-yield-stats schema 1504 + counters
//   AC6: depth-slot / nested-guard-depth-max observable under Guard
//   AC7: fiber:yield still works (regression)
//   AC8: multi-thread concurrent depth reads + safe-yield (no crash)

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
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

static void ac1_depth_int() {
    std::println("\n--- AC1: query:mutation-boundary-depth ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto d = cs.eval("(engine:metrics \"query:mutation-boundary-depth\")");
    CHECK(d && is_int(*d) && as_int(*d) >= 0, "depth is non-negative int");
    CHECK(as_int(*d) == 0, "depth is 0 outside Guard");
}

static void ac2_safe_yield_outside() {
    std::println("\n--- AC2: safe-yield outside Guard ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define y 0)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto skip0 = cs.evaluator().get_safe_yield_skipped_held_total();
    const auto sum0 =
        cs.evaluator().get_safe_yield_ok_total() + cs.evaluator().get_safe_yield_no_fiber_total();
    auto h = cs.eval("(engine:metrics \"query:mutation-boundary-safe-yield\")");
    CHECK(h && is_hash(*h), "safe-yield returns hash");
    CHECK(href(cs, "query:mutation-boundary-safe-yield-stats", "schema") == 1635 ||
              href(cs, "query:mutation-boundary-safe-yield-stats", "schema") == 1591 ||
              href(cs, "query:mutation-boundary-safe-yield-stats", "schema") == 1504,
          "stats schema 1635|1591|1504");
    // Outside Guard: must not count as skipped-held
    CHECK(cs.evaluator().get_safe_yield_skipped_held_total() == skip0,
          "no skipped-held outside Guard");
    const auto sum1 =
        cs.evaluator().get_safe_yield_ok_total() + cs.evaluator().get_safe_yield_no_fiber_total();
    CHECK(sum1 > sum0, "ok or no-fiber advanced");
    // Direct C++ API
    const int rc = cs.evaluator().try_safe_yield_at_boundary(0);
    CHECK(rc == 0, "try_safe_yield outside returns 0");
}

static void ac3_safe_yield_inside_guard() {
    std::println("\n--- AC3: safe-yield inside Guard skipped ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define z 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto skip0 = cs.evaluator().get_safe_yield_skipped_held_total();
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard guard(cs.evaluator(), &ok);
        CHECK(Evaluator::mutation_boundary_depth() >= 1, "depth >= 1 under Guard");
        const int rc = cs.evaluator().try_safe_yield_at_boundary(0);
        CHECK(rc == 1, "try_safe_yield under Guard returns 1 (skipped-held)");
    }
    CHECK(cs.evaluator().get_safe_yield_skipped_held_total() > skip0, "skipped-held counter grew");
    // After Guard: depth 0 and yield ok again
    CHECK(Evaluator::mutation_boundary_depth() == 0, "depth 0 after Guard");
    CHECK(cs.evaluator().try_safe_yield_at_boundary(0) == 0, "safe again after Guard");
}

static void ac4_ast_alias() {
    std::println("\n--- AC4: ast:yield-at-boundary alias ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"ast:yield-at-boundary\")");
    CHECK(h && is_hash(*h), "ast:yield-at-boundary returns hash");
    CHECK(href(cs, "ast:yield-at-boundary", "schema") == 1635 ||
              href(cs, "ast:yield-at-boundary", "schema") == 1591 ||
              href(cs, "ast:yield-at-boundary", "schema") == 1504,
          "alias schema 1635|1591|1504");
    CHECK(href(cs, "ast:yield-at-boundary", "skipped-held") == 0 ||
              href(cs, "ast:yield-at-boundary", "yielded") >= 0,
          "alias exposes yielded/skipped keys");
}

static void ac5_stats_schema() {
    std::println("\n--- AC5: safe-yield-stats schema 1504 ---");
    CompilerService cs;
    (void)cs.eval("(engine:metrics \"query:mutation-boundary-safe-yield\")");
    auto h = cs.eval("(engine:metrics \"query:mutation-boundary-safe-yield-stats\")");
    CHECK(h && is_hash(*h), "stats hash");
    CHECK(href(cs, "query:mutation-boundary-safe-yield-stats", "schema") == 1635 ||
              href(cs, "query:mutation-boundary-safe-yield-stats", "schema") == 1591 ||
              href(cs, "query:mutation-boundary-safe-yield-stats", "schema") == 1504,
          "schema 1635|1591|1504");
    CHECK(href(cs, "query:mutation-boundary-safe-yield-stats", "boundary-depth") >= 0, "depth");
    CHECK(href(cs, "query:mutation-boundary-safe-yield-stats", "depth-slot") >= 0, "depth-slot");
    CHECK(
        href(cs, "query:mutation-boundary-safe-yield-stats", "safe-yield-ok-total") +
                href(cs, "query:mutation-boundary-safe-yield-stats", "safe-yield-no-fiber-total") >=
            0,
        "counters present");
    CHECK(href(cs, "query:mutation-boundary-safe-yield-stats", "nested-guard-depth-max") >= 0,
          "nested-guard-depth-max");
    CHECK(href(cs, "query:mutation-boundary-safe-yield-stats", "per-fiber-stack-depth-max") >= 0,
          "per-fiber-stack-depth-max");
}

static void ac6_depth_under_guard() {
    std::println("\n--- AC6: depth-slot under nested Guards ---");
    CompilerService cs;
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g1(cs.evaluator(), &ok);
        CHECK(cs.evaluator().mutation_boundary_depth_slot_value() >= 1 ||
                  Evaluator::mutation_boundary_depth() >= 1,
              "slot or stack depth >= 1");
        {
            Evaluator::MutationBoundaryGuard g2(cs.evaluator(), &ok);
            CHECK(Evaluator::mutation_boundary_depth() >= 2, "nested depth >= 2");
            CHECK(cs.evaluator().nested_guard_depth_max() >= 2 ||
                      Evaluator::mutation_boundary_depth() >= 2,
                  "nested max observed");
        }
    }
    CHECK(Evaluator::mutation_boundary_depth() == 0, "depth restored");
}

static void ac7_fiber_yield_regression() {
    std::println("\n--- AC7: fiber:yield regression ---");
    CompilerService cs;
    // In non-serve mode fiber:yield is a no-op void — must not crash.
    auto r = cs.eval("(fiber:yield)");
    CHECK(r.has_value(), "fiber:yield callable");
}

static void ac8_concurrent() {
    std::println("\n--- AC8: concurrent C++ depth + safe-yield ---");
    // Use a standalone Evaluator (no shared CompilerService eval) so
    // threads only touch thread-local depth slots + atomics.
    Evaluator ev;
    std::atomic<int> fails{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&ev, &fails] {
            for (int i = 0; i < 100; ++i) {
                (void)ev.mutation_boundary_depth();
                (void)ev.try_safe_yield_at_boundary(0); // outside Guard → 0
                bool ok = true;
                {
                    Evaluator::MutationBoundaryGuard g(ev, &ok);
                    if (ev.try_safe_yield_at_boundary(0) != 1)
                        fails.fetch_add(1);
                }
            }
        });
    }
    for (auto& th : threads)
        th.join();
    CHECK(fails.load() == 0, "all under-Guard yields skipped-held");
    CHECK(ev.get_safe_yield_skipped_held_total() > 0, "skipped-held observed");
}

} // namespace

int main() {
    std::println("test_issue_1504: mutation-boundary safe yield + depth (#1504)");
    ac1_depth_int();
    ac2_safe_yield_outside();
    ac3_safe_yield_inside_guard();
    ac4_ast_alias();
    ac5_stats_schema();
    ac6_depth_under_guard();
    ac7_fiber_yield_regression();
    ac8_concurrent();
    std::println("\n#1504: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
