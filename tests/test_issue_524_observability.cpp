// @category: integration
// @reason: Issue #524 — macro-production-hygiene-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_524_detail {
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

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:macro-production-hygiene-stats\") '{}')", key));
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

} // namespace aura_issue_524_detail

int aura_issue_524_observability_run() {
    using namespace aura_issue_524_detail;

    std::println("=== Issue #524: macro-production-hygiene-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_macro_workspace(cs), "hygienic macro workspace setup");

    // AC1: query:macro-production-hygiene-stats returns hash
    {
        std::println("\n--- AC1: query:macro-production-hygiene-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:macro-production-hygiene-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:macro-production-hygiene-stats returns hash");
        CHECK(hash_int(cs, "root-skips") >= 0, "root-skips present");
        CHECK(hash_int(cs, "recursive-skips") >= 0, "recursive-skips present");
        CHECK(hash_int(cs, "hygiene-violations") >= 0, "hygiene-violations present");
        CHECK(hash_int(cs, "filter-violations") >= 0, "filter-violations present");
        CHECK(hash_int(cs, "macro-markers") >= 0, "macro-markers present");
        CHECK(hash_int(cs, "inline-hygiene-skipped") >= 0, "inline-hygiene-skipped present");
        CHECK(hash_int(cs, "respect-macro-hygiene") >= 0, "respect-macro-hygiene present");
        CHECK(hash_int(cs, "contract-violations") >= 0, "contract-violations present");
        CHECK(hash_int(cs, "macro-expansion-dirty") >= 0, "macro-expansion-dirty present");
        CHECK(hash_int(cs, "hygiene-filter-rate-pct") >= 0, "hygiene-filter-rate-pct present");
        CHECK(hash_int(cs, "macro-production-hygiene-total") >= 0,
              "macro-production-hygiene-total present");
        CHECK(hash_int(cs, "macro-production-hygiene-recommendation") >= 0,
              "macro-production-hygiene-recommendation present");
    }

    const auto total_before = hash_int(cs, "macro-production-hygiene-total");
    const auto root_before = hash_int(cs, "root-skips");

    // AC2: evaluator bump accessors increase production counters
    {
        std::println("\n--- AC2: hygiene bump accessors ---");
        auto& ev = cs.evaluator();
        ev.bump_macro_introduced_skipped_in_query();
        ev.bump_pattern_recursive_macro_skipped(2);
        ev.bump_hygiene_violation_count();
        const auto root_after = hash_int(cs, "root-skips");
        CHECK(root_after > root_before,
              std::format("root-skips grew ({} -> {})", root_before, root_after));
        CHECK(hash_int(cs, "recursive-skips") >= 2, "recursive-skips bumped");
        CHECK(hash_int(cs, "hygiene-violations") >= 1, "hygiene-violations bumped");
    }

    // AC3: query:pattern hygiene filter + mutate cycle
    {
        std::println("\n--- AC3: pattern hygiene + mutate cycle ---");
        const auto default_cnt = result_count(cs, "(query:pattern \"*\")");
        const auto allow_cnt = result_count(cs, "(query:pattern \"*\" :allow-macro-introduced #t)");
        CHECK(default_cnt >= 0 && allow_cnt >= 0, "pattern match counts observable");
        CHECK(allow_cnt >= default_cnt, "allow-macro-introduced yields >= default-filtered");
        CHECK(cs.eval("(mutate:rebind \"base\" \"99\")").has_value(), "mutate:rebind under Guard");
        (void)cs.eval("(query:pattern \"base\")");
        (void)cs.eval("(query:pattern \"*\" :respect-hygiene #t)");
        const auto total_after = hash_int(cs, "macro-production-hygiene-total");
        CHECK(total_after >= total_before,
              std::format("macro-production-hygiene-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related macro/hygiene primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto pms = cs.eval("(engine:metrics \"query:pattern-marker-stats\")");
        auto ihs = cs.eval("(engine:metrics \"query:ir-hygiene-stats\")");
        auto phs = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
        CHECK(pms && aura::compiler::types::is_hash(*pms), "pattern-marker-stats hash regression");
        CHECK(ihs && aura::compiler::types::is_hash(*ihs), "ir-hygiene-stats hash regression");
        CHECK(phs && aura::compiler::types::is_int(*phs), "pattern-hygiene-stats int regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 121,
              "stats:count >= 121");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_524_observability_run();
}
#endif
