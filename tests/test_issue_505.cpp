// @category: integration
// @reason: Issue #505 — closure/EnvFrame/bridge_epoch post-invalidate safety stats

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_505_detail {
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

static bool setup_closure_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (f x) x) (define a 1) (f 42)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_505_detail

int aura_issue_505_run() {
    using namespace aura_issue_505_detail;

    std::println("=== Issue #505: closure-env-safety-stats hash + post-invalidate ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_closure_workspace(cs), "closure workspace setup");

    const auto sr0 = cs.get_closure_stale_refresh_count();
    const auto bh0 = cs.get_bridge_epoch_hit_count();
    const auto lp0 = cs.get_linear_check_pass_count();
    const auto gs0 = cs.get_gc_envframe_stale_skipped();

    // AC1: query:closure-env-safety-stats returns hash
    {
        std::println("\n--- AC1: query:closure-env-safety-stats ---");
        auto stats = cs.eval("(query:closure-env-safety-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:closure-env-safety-stats returns hash");
    }

    // AC2: closure call + mutate path — counters observable
    {
        std::println("\n--- AC2: mutate + closure call ---");
        for (int i = 0; i < 5; ++i) {
            (void)cs.eval("(f 42)");
        }
        CHECK(cs.eval("(mutate:replace-value (define a 99) (define a 99))").has_value(),
              "mutate under Guard");
        (void)cs.eval("(f 42)");
        const auto sr1 = cs.get_closure_stale_refresh_count();
        const auto bh1 = cs.get_bridge_epoch_hit_count();
        CHECK(sr1 >= sr0, std::format("stale_refresh non-decreasing ({} -> {})", sr0, sr1));
        CHECK(bh1 >= bh0, std::format("bridge_hit non-decreasing ({} -> {})", bh0, bh1));
        auto stats = cs.eval("(query:closure-env-safety-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "closure-env-safety-stats hash after mutate");
    }

    // AC3: CompilerService bump accessors
    {
        std::println("\n--- AC3: bump accessors ---");
        cs.bump_bridge_epoch_hit_count();
        cs.bump_linear_check_pass_count();
        cs.bump_gc_envframe_stale_skipped();
        const auto bh2 = cs.get_bridge_epoch_hit_count();
        const auto lp1 = cs.get_linear_check_pass_count();
        const auto gs1 = cs.get_gc_envframe_stale_skipped();
        CHECK(bh2 > bh0, "bridge_epoch_hit bumped");
        CHECK(lp1 > lp0, "linear_check_pass bumped");
        CHECK(gs1 > gs0, "gc_envframe_stale_skipped bumped");
    }

    // AC4: mutate-then-query self-evolution cycle
    {
        std::println("\n--- AC4: mutate-then-query cycle ---");
        CHECK(cs.eval("(mutate:rebind \"a\" \"10\")").has_value(), "mutate:rebind under Guard");
        (void)cs.eval("(query:pattern \"a\")");
        (void)cs.eval("(f 42)");
        auto stats = cs.eval("(query:closure-env-safety-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "closure-env-safety-stats hash after query cycle");
    }

    // AC5: query-only path zero regression
    {
        std::println("\n--- AC5: query-only regression ---");
        aura::compiler::CompilerService cs2;
        CHECK(setup_closure_workspace(cs2), "fresh workspace for query-only path");
        const auto sr_q0 = cs2.get_closure_stale_refresh_count();
        (void)cs2.eval("(query:pattern \"a\")");
        auto stats = cs2.eval("(query:closure-env-safety-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "closure-env-safety-stats hash on query-only path");
        CHECK(cs2.get_closure_stale_refresh_count() == sr_q0,
              "stale_refresh unchanged on query-only path");
    }

    // AC6: related primitive regression
    {
        std::println("\n--- AC6: related stats regression ---");
        auto p6 = cs.eval("(query:prompt6-safety-score)");
        auto ef = cs.eval("(query:envframe-dualpath-stats)");
        CHECK(p6 && aura::compiler::types::is_int(*p6), "prompt6-safety-score regression");
        CHECK(ef && aura::compiler::types::is_int(*ef), "envframe-dualpath-stats regression");
    }

    // AC7: stats:count
    {
        std::println("\n--- AC7: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 101,
              "stats:count >= 101");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_505_run();
}
#endif
