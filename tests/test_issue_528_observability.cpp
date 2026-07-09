// @category: integration
// @reason: Issue #528 — pattern-production-index-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_528_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:pattern-production-index-stats) '{}')", key));
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

} // namespace aura_issue_528_detail

int aura_issue_528_observability_run() {
    using namespace aura_issue_528_detail;

    std::println("=== Issue #528: pattern-production-index-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_macro_workspace(cs), "hygienic macro workspace setup");

    // AC1: query:pattern-production-index-stats returns hash
    {
        std::println("\n--- AC1: query:pattern-production-index-stats ---");
        auto stats = cs.eval("(query:pattern-production-index-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:pattern-production-index-stats returns hash");
        CHECK(hash_int(cs, "index-hits") >= 0, "index-hits present");
        CHECK(hash_int(cs, "index-misses") >= 0, "index-misses present");
        CHECK(hash_int(cs, "index-rebuilds") >= 0, "index-rebuilds present");
        CHECK(hash_int(cs, "dirty-marks") >= 0, "dirty-marks present");
        CHECK(hash_int(cs, "rebuild-time-us") >= 0, "rebuild-time-us present");
        CHECK(hash_int(cs, "delta-hits") >= 0, "delta-hits present");
        CHECK(hash_int(cs, "lazy-rebuilds") >= 0, "lazy-rebuilds present");
        CHECK(hash_int(cs, "eager-mutate-rebuilds") >= 0, "eager-mutate-rebuilds present");
        CHECK(hash_int(cs, "eager-cow-rebuilds") >= 0, "eager-cow-rebuilds present");
        CHECK(hash_int(cs, "structural-hits") >= 0, "structural-hits present");
        CHECK(hash_int(cs, "structural-misses") >= 0, "structural-misses present");
        CHECK(hash_int(cs, "index-entries") >= 0, "index-entries present");
        CHECK(hash_int(cs, "root-skips") >= 0, "root-skips present");
        CHECK(hash_int(cs, "recursive-skips") >= 0, "recursive-skips present");
        CHECK(hash_int(cs, "hygiene-violations") >= 0, "hygiene-violations present");
        CHECK(hash_int(cs, "macro-markers") >= 0, "macro-markers present");
        CHECK(hash_int(cs, "arity-accuracy-pct") >= 0, "arity-accuracy-pct present");
        CHECK(hash_int(cs, "delta-hit-rate-pct") >= 0, "delta-hit-rate-pct present");
        CHECK(hash_int(cs, "pattern-production-index-total") >= 0,
              "pattern-production-index-total present");
        CHECK(hash_int(cs, "pattern-production-index-recommendation") >= 0,
              "pattern-production-index-recommendation present");
    }

    const auto total_before = hash_int(cs, "pattern-production-index-total");
    const auto root_before = hash_int(cs, "root-skips");

    // AC2: query:pattern drives index + hygiene counters
    {
        std::println("\n--- AC2: pattern query + hygiene filter ---");
        const auto default_cnt = result_count(cs, "(query:pattern \"*\")");
        const auto allow_cnt = result_count(cs, "(query:pattern \"*\" :allow-macro-introduced #t)");
        CHECK(default_cnt >= 0 && allow_cnt >= 0, "pattern match counts observable");
        CHECK(allow_cnt >= default_cnt, "allow-macro-introduced yields >= default-filtered");
        const auto root_after = hash_int(cs, "root-skips");
        CHECK(root_after >= root_before,
              std::format("root-skips non-decreasing ({} -> {})", root_before, root_after));
        CHECK(hash_int(cs, "index-hits") >= 0 || hash_int(cs, "structural-hits") >= 0,
              "index or structural hits observable after pattern query");
    }

    // AC3: mutate + pattern cycle
    {
        std::println("\n--- AC3: mutate + pattern cycle ---");
        CHECK(cs.eval("(mutate:rebind \"base\" \"99\")").has_value(), "mutate:rebind under Guard");
        (void)cs.eval("(query:pattern \"base\")");
        (void)cs.eval("(query:pattern \"*\" :respect-hygiene #t)");
        const auto total_after = hash_int(cs, "pattern-production-index-total");
        CHECK(total_after >= total_before,
              std::format("pattern-production-index-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related pattern index/hygiene primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto pis = cs.eval("(query:pattern-index-stats)");
        auto phs = cs.eval("(query:pattern-hygiene-stats)");
        auto pish = cs.eval("(query:pattern-index-stats-hash)");
        CHECK(pis && aura::compiler::types::is_int(*pis), "pattern-index-stats int regression");
        CHECK(phs && aura::compiler::types::is_int(*phs), "pattern-hygiene-stats int regression");
        CHECK(pish && aura::compiler::types::is_hash(*pish),
              "pattern-index-stats-hash hash regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 123,
              "stats:count >= 123");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_528_observability_run();
}
#endif
