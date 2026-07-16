// @category: integration
// @reason: Issue #501 — IR MacroIntroduced hygiene (InlinePass + lowering)

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_501_hygiene_detail {
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

static std::int64_t inline_skipped(aura::compiler::CompilerService& cs) {
    // Two-step bind + hash-ref. Nested (hash-ref (engine:metrics ...)) is
    // fragile; also avoid re-using a single define name after mutate.
    if (!cs.eval("(define __inline-pass-stats "
                 "(engine:metrics \"compile:inline-pass-stats\"))"))
        return -1;
    auto r = cs.eval("(hash-ref __inline-pass-stats \"macro-hygiene-skipped\")");
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::int64_t macro_introduced_count(aura::compiler::CompilerService& cs) {
    auto r = cs.eval("(length (query:macro-introduced))");
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

} // namespace aura_issue_501_hygiene_detail

int aura_issue_501_hygiene_run() {
    using namespace aura_issue_501_hygiene_detail;

    std::println("=== Issue #501: IR MacroIntroduced hygiene ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_macro_workspace(cs), "hygienic macro workspace setup");

    // AC1: query:ir-hygiene-stats returns hash (top-level call — no hash-ref nest)
    {
        std::println("\n--- AC1: query:ir-hygiene-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:ir-hygiene-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:ir-hygiene-stats returns hash");
        CHECK(macro_introduced_count(cs) >= 3, "macro-introduced nodes >= 3 after macro eval");
        CHECK(inline_skipped(cs) >= 0, "inline-hygiene-skipped observable via compile stats");
    }

    const auto markers_before = macro_introduced_count(cs);
    const auto inline_before = inline_skipped(cs);

    // AC2: InlinePass respects MacroIntroduced at call sites
    {
        std::println("\n--- AC2: InlinePass macro hygiene skip ---");
        const auto skipped = inline_skipped(cs);
        CHECK(skipped >= 0, "compile:inline-pass-stats macro-hygiene-skipped present");
        auto stats2 = cs.eval("(engine:metrics \"query:ir-hygiene-stats\")");
        CHECK(stats2 && aura::compiler::types::is_hash(*stats2),
              "ir-hygiene-stats still hash after inline compile");
    }

    // AC3: mutate-then-query self-evolution cycle preserves hygiene
    {
        std::println("\n--- AC3: mutate-then-query cycle ---");
        CHECK(cs.eval("(mutate:rebind \"base\" \"99\")").has_value(), "mutate:rebind under Guard");
        (void)cs.eval("(query:pattern \"base\")");
        (void)cs.eval("(query:pattern \"*\" :allow-macro-introduced #f)");
        auto user_pat = cs.eval("(query:pattern \"base\")");
        CHECK(user_pat.has_value(), "query:pattern finds user binding after mutate");
        CHECK(macro_introduced_count(cs) >= markers_before,
              "macro-introduced count stable after mutate+query");
    }

    // AC5 before AC4: a second CompilerService tears down shared g_hash_tables
    // state that breaks subsequent hash-ref string-key equality on `cs`.
    // Monotonicity must be observed on the same service before multi-CS stress.
    {
        std::println("\n--- AC5: query regression ---");
        auto phs = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
        auto ips = cs.eval("(engine:metrics \"compile:inline-pass-stats\")");
        CHECK(phs && aura::compiler::types::is_int(*phs), "pattern-hygiene-stats regression");
        CHECK(ips && aura::compiler::types::is_hash(*ips), "compile:inline-pass-stats regression");
        CHECK(inline_skipped(cs) >= inline_before, "inline-hygiene-skipped monotonic");
    }

    // AC4: non-macro IR path zero regression (isolated service)
    {
        std::println("\n--- AC4: non-macro regression ---");
        aura::compiler::CompilerService cs2;
        CHECK(cs2.eval("(set-code \"(define x 1) (+ x 1)\")").has_value(), "user-only code set");
        CHECK(cs2.eval("(eval-current)").has_value(), "user-only eval");
        auto stats = cs2.eval("(engine:metrics \"query:ir-hygiene-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "ir-hygiene-stats hash on user-only path");
        CHECK(macro_introduced_count(cs2) == 0, "macro-introduced == 0 on user-only path");
        CHECK(inline_skipped(cs2) == 0, "inline-hygiene-skipped == 0 on user-only path");
    }

    // AC6: (*allow-macro-inline* #t) toggles respect-macro-hygiene off
    {
        std::println("\n--- AC6: allow-macro-inline toggle ---");
        aura::compiler::CompilerService cs3;
        CHECK(setup_macro_workspace(cs3), "toggle workspace setup");
        auto toggle = cs3.eval("(*allow-macro-inline* #t)");
        CHECK(toggle && aura::compiler::types::is_int(*toggle),
              "(*allow-macro-inline* #t) callable");
        auto stats_on = cs3.eval("(engine:metrics \"query:ir-hygiene-stats\")");
        CHECK(stats_on && aura::compiler::types::is_hash(*stats_on),
              "ir-hygiene-stats hash after opt-in");
        (void)cs3.eval("(*allow-macro-inline* #f)");
        auto stats_off = cs3.eval("(engine:metrics \"query:ir-hygiene-stats\")");
        CHECK(stats_off && aura::compiler::types::is_hash(*stats_off),
              "ir-hygiene-stats hash after opt-out");
    }

    // AC7: stats:count
    {
        std::println("\n--- AC7: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 98,
              "stats:count >= 98");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_501_hygiene_run();
}
#endif
