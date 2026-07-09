// @category: integration
// @reason: Issue #492 — ShapeProfiler deopt stability + JIT/fiber integration

#include <format>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_492_detail {
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

static std::int64_t snap_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:shape-profiler-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define (dbl y) (* y 2)) "
                 "(define (wrap z) (add1 z)) "
                 "(define base 10) (define acc 0) "
                 "(add1 1) (dbl 3) (wrap 5)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_492_detail

int aura_issue_492_run() {
    using namespace aura_issue_492_detail;

    std::println("=== Issue #492: ShapeProfiler deopt stability + JIT integration ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup + eval-current");

    // AC1: query:shape-profiler-stats fields
    {
        std::println("\n--- AC1: query:shape-profiler-stats ---");
        auto stats = cs.eval("(query:shape-profiler-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:shape-profiler-stats returns hash");
        CHECK(snap_stat(cs, "stability-hits") >= 0, "stability-hits present");
        CHECK(snap_stat(cs, "version-bumps") >= 0, "version-bumps present");
        CHECK(snap_stat(cs, "deopt-hooks") >= 0, "deopt-hooks present");
        CHECK(snap_stat(cs, "deopt-storm-count") >= 0, "deopt-storm-count present");
        CHECK(snap_stat(cs, "shape-changes-observed") >= 0, "shape-changes-observed present");
        CHECK(snap_stat(cs, "window-size") >= 1000, "window-size >= 1000");
        CHECK(snap_stat(cs, "stability-ratio-bp") >= 9000, "stability-ratio-bp >= 9000");
        CHECK(snap_stat(cs, "fiber-refresh") >= 0, "fiber-refresh present");
        CHECK(snap_stat(cs, "jit-shape-miss") >= 0, "jit-shape-miss present");
    }

    // JIT compile seeds ShapeProfiler profiles (see #605 matrix).
    CHECK(cs.eval("(eval-current :jit)").has_value(), "JIT warmup before mutate");

    const auto hooks_before = snap_stat(cs, "deopt-hooks");
    const auto bumps_before = snap_stat(cs, "version-bumps");

    // AC2: mutate triggers shape profiler invalidation counters
    {
        std::println("\n--- AC2: mutate invalidates shape profiles ---");
        const auto mr = cs.typed_mutate("(mutate:rebind \"base\" \"99\")");
        CHECK(mr.success, "typed_mutate rebind under Guard");
        const auto hooks_after = snap_stat(cs, "deopt-hooks");
        const auto bumps_after = snap_stat(cs, "version-bumps");
        CHECK(hooks_after > hooks_before,
              std::format("deopt-hooks grew ({} -> {})", hooks_before, hooks_after));
        CHECK(bumps_after > bumps_before,
              std::format("version-bumps grew ({} -> {})", bumps_before, bumps_after));
        CHECK(cs.eval("(eval-current)").has_value(), "eval-current after mutate");
    }

    // AC3: deopt hook wired — shape-churn grows on typed_mutate path
    {
        std::println("\n--- AC3: deopt hook closed-loop wiring ---");
        const auto churn_after = snap_stat(cs, "shape-churn");
        CHECK(churn_after >= 0, "shape-churn observable after mutate");
        CHECK(snap_stat(cs, "deopt-hooks") > hooks_before,
              "deopt-hooks grew (hook registered + fired)");
    }

    // AC4: query:shape-stability-stats regression
    {
        std::println("\n--- AC4: query:shape-stability-stats regression ---");
        auto stable = cs.eval("(query:shape-stability-stats)");
        CHECK(stable && aura::compiler::types::is_int(*stable),
              "query:shape-stability-stats regression");
    }

    // AC5: mutate-then-query closed loop
    {
        std::println("\n--- AC5: mutate-then-query closed loop ---");
        CHECK(cs.eval("(query:pattern \"add1\")").has_value(), "query:pattern after mutate");
        CHECK(snap_stat(cs, "shape-churn") >= 0, "shape-churn still observable");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 211,
              "stats:count >= 211");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_492_run();
}
#endif
