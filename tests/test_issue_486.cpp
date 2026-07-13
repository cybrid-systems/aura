// @category: integration
// @reason: Issue #486 — query:pattern MacroIntroduced filter + macro-hygiene-stats

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_486_detail {
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

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:macro-hygiene-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

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

} // namespace aura_issue_486_detail

int aura_issue_486_run() {
    using namespace aura_issue_486_detail;

    std::println("=== Issue #486: query:pattern MacroIntroduced hygiene ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_macro_workspace(cs), "hygienic macro workspace setup");

    // AC1: query:macro-hygiene-stats hash fields
    {
        std::println("\n--- AC1: query:macro-hygiene-stats ---");
        auto stats = cs.eval("(query:macro-hygiene-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:macro-hygiene-stats returns hash");
        CHECK(stat_int(cs, "root-skips") >= 0, "root-skips present");
        CHECK(stat_int(cs, "recursive-skips") >= 0, "recursive-skips present");
        CHECK(stat_int(cs, "hygiene-violations") >= 0, "hygiene-violations present");
        CHECK(stat_int(cs, "macro-markers") >= 3, "macro-markers >= 3 after macro eval");
    }

    const auto root_before = stat_int(cs, "root-skips");
    const auto default_cnt = result_count(cs, "(query:pattern \"*\")");
    const auto include_cnt = result_count(cs, "(query:pattern \"*\" :include-macro-introduced #t)");
    const auto allow_cnt = result_count(cs, "(query:pattern \"*\" :allow-macro-introduced #t)");
    const auto deny_cnt = result_count(cs, "(query:pattern \"*\" :allow-macro-introduced #f)");

    // AC2: :allow-macro-introduced mirrors :include-macro-introduced
    {
        std::println("\n--- AC2: :allow-macro-introduced flag syntax ---");
        CHECK(default_cnt >= 0 && include_cnt >= 0 && allow_cnt >= 0 && deny_cnt >= 0,
              "pattern match counts observable");
        CHECK(allow_cnt == include_cnt,
              std::format(":allow-macro-introduced #t == :include-macro-introduced #t ({} vs {})",
                          allow_cnt, include_cnt));
        CHECK(deny_cnt == default_cnt,
              std::format(":allow-macro-introduced #f == default hygiene ({} vs {})", deny_cnt,
                          default_cnt));
        CHECK(allow_cnt >= default_cnt, "allow/include yields >= default hygiene-filtered matches");
    }

    // AC3: default hygiene skips MacroIntroduced roots
    {
        std::println("\n--- AC3: default query:pattern hygiene filter ---");
        const auto root_after = stat_int(cs, "root-skips");
        CHECK(
            root_after > root_before,
            std::format("root-skips grew after query:pattern ({} -> {})", root_before, root_after));
    }

    // AC4: mutate-then-query self-evolution cycle
    {
        std::println("\n--- AC4: mutate-then-query cycle ---");
        const auto markers_before = stat_int(cs, "macro-markers");
        CHECK(cs.eval("(mutate:rebind \"base\" \"99\")").has_value(), "mutate:rebind under Guard");
        (void)cs.eval("(query:pattern \"base\")");
        (void)cs.eval("(query:pattern \"*\" :allow-macro-introduced #f)");
        CHECK(cs.eval("(eval-current)").has_value(), "eval-current after mutate");
        const auto markers_after = stat_int(cs, "macro-markers");
        CHECK(markers_after >= markers_before,
              std::format("macro-markers monotonic ({} -> {})", markers_before, markers_after));
        auto user_pat = cs.eval("(query:pattern \"base\")");
        CHECK(user_pat.has_value(), "query:pattern finds user binding after mutate");
    }

    // AC5: zero regression on existing marker queries
    {
        std::println("\n--- AC5: marker query regression ---");
        auto macro_n = cs.eval("(length (query:macro-introduced))");
        auto by_marker = cs.eval("(length (query:by-marker \"MacroIntroduced\"))");
        auto phs = cs.eval("(query:pattern-hygiene-stats)");
        auto hys = cs.eval("(query:hygiene-stats)");
        CHECK(macro_n && aura::compiler::types::is_int(*macro_n), "macro-introduced reachable");
        CHECK(by_marker && aura::compiler::types::is_int(*by_marker), "by-marker reachable");
        CHECK(phs && aura::compiler::types::is_int(*phs), "pattern-hygiene-stats regression");
        CHECK(hys && aura::compiler::types::is_int(*hys), "hygiene-stats regression");
        if (macro_n && by_marker && aura::compiler::types::is_int(*macro_n) &&
            aura::compiler::types::is_int(*by_marker)) {
            CHECK(aura::compiler::types::as_int(*macro_n) ==
                      aura::compiler::types::as_int(*by_marker),
                  "macro-introduced matches by-marker MacroIntroduced");
        }
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
    return aura_issue_486_run();
}
#endif
