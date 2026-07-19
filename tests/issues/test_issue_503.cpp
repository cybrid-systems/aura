// @category: integration
// @reason: Issue #503 — query:pattern hygiene flags + pattern-marker-stats

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_503_detail {
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

static std::int64_t result_count(aura::compiler::CompilerService& cs, const std::string& expr) {
    auto r = cs.eval("(length " + expr + ")");
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_macro_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (d y) (* y 2)) "
                 "(d 1) (d 2) (d 3) "
                 "(define base 10) (+ base 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_503_detail

int aura_issue_503_run() {
    using namespace aura_issue_503_detail;

    std::println("=== Issue #503: query:pattern hygiene + pattern-marker-stats ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_macro_workspace(cs), "hygienic macro workspace setup");

    const auto root_skips_before = cs.evaluator().get_macro_introduced_skipped_in_query();
    const auto default_cnt = result_count(cs, "(query:pattern \"*\")");
    const auto allow_cnt = result_count(cs, "(query:pattern \"*\" :allow-macro-introduced #t)");
    const auto respect_off = result_count(cs, "(query:pattern \"*\" :respect-hygiene #f)");
    const auto respect_on = result_count(cs, "(query:pattern \"*\" :respect-hygiene #t)");

    // AC1: query:pattern-marker-stats returns hash
    {
        std::println("\n--- AC1: query:pattern-marker-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:pattern-marker-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:pattern-marker-stats returns hash");
    }

    // AC2: :allow-macro-introduced and :respect-hygiene flags
    {
        std::println("\n--- AC2: hygiene flag syntax ---");
        CHECK(default_cnt >= 0 && allow_cnt >= 0 && respect_off >= 0 && respect_on >= 0,
              "pattern match counts observable");
        CHECK(allow_cnt == respect_on,
              std::format(":allow-macro-introduced #t == :respect-hygiene #t ({} vs {})", allow_cnt,
                          respect_on));
        CHECK(respect_off == default_cnt,
              std::format(":respect-hygiene #f == default hygiene ({} vs {})", respect_off,
                          default_cnt));
        CHECK(allow_cnt >= default_cnt, "allow/respect-on yields >= default-filtered matches");
    }

    // AC3: default hygiene skips MacroIntroduced roots
    {
        std::println("\n--- AC3: default hygiene filter ---");
        const auto root_skips_after = cs.evaluator().get_macro_introduced_skipped_in_query();
        CHECK(root_skips_after > root_skips_before,
              std::format("root-skips grew ({} -> {})", root_skips_before, root_skips_after));
    }

    // AC4: mutate-then-query self-evolution cycle
    {
        std::println("\n--- AC4: mutate-then-query cycle ---");
        const auto markers_before = cs.evaluator().get_macro_markers_in_snapshot();
        CHECK(cs.eval("(mutate:rebind \"base\" \"99\")").has_value(), "mutate:rebind under Guard");
        (void)cs.eval("(query:pattern \"base\")");
        (void)cs.eval("(query:pattern \"*\" :allow-macro-introduced #f)");
        auto user_pat = cs.eval("(query:pattern \"base\")");
        CHECK(user_pat.has_value(), "query:pattern finds user binding after mutate");
        auto stats = cs.eval("(engine:metrics \"query:pattern-marker-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "pattern-marker-stats hash after cycle");
        CHECK(cs.evaluator().get_macro_markers_in_snapshot() >= markers_before,
              "macro-markers snapshot monotonic");
    }

    // AC5: non-macro query path zero regression
    {
        std::println("\n--- AC5: user-only regression ---");
        aura::compiler::CompilerService cs2;
        CHECK(cs2.eval("(set-code \"(define x 1) (+ x 1)\")").has_value(), "user-only code set");
        CHECK(cs2.eval("(eval-current)").has_value(), "user-only eval");
        const auto cnt = result_count(cs2, "(query:pattern \"x\")");
        CHECK(cnt >= 0, "query:pattern works on user-only path");
        auto stats = cs2.eval("(engine:metrics \"query:pattern-marker-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "pattern-marker-stats hash on user-only path");
    }

    // AC6: related primitive regression (fresh service — avoid post-mutate workspace scan)
    {
        std::println("\n--- AC6: query regression ---");
        aura::compiler::CompilerService cs3;
        CHECK(setup_macro_workspace(cs3), "regression workspace setup");
        auto phs = cs3.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
        auto pms = cs3.eval("(engine:metrics \"query:pattern-marker-stats\")");
        CHECK(phs && (aura::compiler::types::is_int(*phs) || aura::compiler::types::is_hash(*phs)),
              "pattern-hygiene-stats regression");
        CHECK(pms && aura::compiler::types::is_hash(*pms), "pattern-marker-stats regression");
    }

    // AC7: stats:count
    {
        std::println("\n--- AC7: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 100,
              "stats:count >= 100");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_503_run();
}
#endif
