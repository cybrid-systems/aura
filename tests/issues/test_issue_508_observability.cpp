// @category: integration
// @reason: Issue #508 — dead-coercion-zerooverhead-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_508_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static bool setup_gradual_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 42) (define y \\\"hello\\\") (define z #t)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_508_detail

int aura_issue_508_observability_run() {
    using namespace aura_issue_508_detail;

    std::println("=== Issue #508: dead-coercion-zerooverhead-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_gradual_workspace(cs), "gradual workspace setup");

    const auto elim0 = cs.snapshot().dead_coercion_eliminated_total;
    const auto elapsed0 = cs.snapshot().dead_coercion_elapsed_us_total;

    // AC1: query:dead-coercion-zerooverhead-stats returns hash
    {
        std::println("\n--- AC1: query:dead-coercion-zerooverhead-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:dead-coercion-zerooverhead-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:dead-coercion-zerooverhead-stats returns hash");
    }

    // AC2: compile: primitives still return int
    {
        std::println("\n--- AC2: compile:dead-coercion primitives ---");
        auto dcs = cs.eval("(engine:metrics \"compile:dead-coercion-stats\")");
        auto elapsed = cs.eval("(stats:get \"compile:dead-coercion-elapsed\")");
        auto kept = cs.eval("(stats:get \"compile:dead-coercion-kept-for-debug\")");
        CHECK(dcs && aura::compiler::types::is_int(*dcs), "compile:dead-coercion-stats int");
        CHECK(elapsed && aura::compiler::types::is_int(*elapsed),
              "compile:dead-coercion-elapsed int");
        CHECK(kept && aura::compiler::types::is_int(*kept),
              "compile:dead-coercion-kept-for-debug int");
    }

    // AC3: mutate-then-eval bumps elimination counters
    {
        std::println("\n--- AC3: mutate-then-eval cycle ---");
        CHECK(cs.eval("(mutate:rebind \"x\" \"99\")").has_value(), "mutate:rebind under Guard");
        CHECK(cs.eval("(eval-current)").has_value(), "post-mutate eval");
        const auto elim1 = cs.snapshot().dead_coercion_eliminated_total;
        const auto elapsed1 = cs.snapshot().dead_coercion_elapsed_us_total;
        CHECK(elim1 >= elim0, std::format("eliminated monotonic ({} -> {})", elim0, elim1));
        CHECK(elapsed1 >= elapsed0,
              std::format("elapsed-us monotonic ({} -> {})", elapsed0, elapsed1));
        auto stats = cs.eval("(engine:metrics \"query:dead-coercion-zerooverhead-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "zerooverhead-stats hash after cycle");
    }

    // AC4: query-only path regression
    {
        std::println("\n--- AC4: query-only regression ---");
        aura::compiler::CompilerService cs2;
        CHECK(setup_gradual_workspace(cs2), "fresh workspace");
        const auto elim_q0 = cs2.snapshot().dead_coercion_eliminated_total;
        auto stats = cs2.eval("(engine:metrics \"query:dead-coercion-zerooverhead-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "zerooverhead-stats hash on query-only path");
        CHECK(cs2.snapshot().dead_coercion_eliminated_total == elim_q0,
              "eliminated unchanged on query-only path");
    }

    // AC5: related primitive regression
    {
        std::println("\n--- AC5: related stats regression ---");
        auto czs = cs.eval("(engine:metrics \"query:coercion-zerooverhead-stats\")");
        auto ces = cs.eval("(engine:metrics \"query:coercion-elim-stats\")");
        CHECK(czs && aura::compiler::types::is_int(*czs), "coercion-zerooverhead-stats regression");
        CHECK(ces && aura::compiler::types::is_int(*ces), "coercion-elim-stats regression");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 102,
              "stats:count >= 102");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_508_observability_run();
}
#endif
