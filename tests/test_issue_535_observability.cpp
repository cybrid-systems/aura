// @category: integration
// @reason: Issue #535 — contracts-production-hotpath-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_535_detail {
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
        "(hash-ref (engine:metrics \"query:contracts-production-hotpath-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (f x) (+ x 1)) (define base 10) (f base)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_535_detail

int aura_issue_535_observability_run() {
    using namespace aura_issue_535_detail;

    std::println("=== Issue #535: contracts-production-hotpath-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup + eval-current");

    // AC1: query:contracts-production-hotpath-stats returns hash
    {
        std::println("\n--- AC1: query:contracts-production-hotpath-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:contracts-production-hotpath-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:contracts-production-hotpath-stats returns hash");
        CHECK(hash_int(cs, "contract-site-count") == 6, "contract-site-count == 6");
        CHECK(hash_int(cs, "shape-dispatch-table-size") == 5, "shape-dispatch-table-size == 5");
        CHECK(hash_int(cs, "consteval-hits") == 30, "consteval-hits == 30");
        CHECK(hash_int(cs, "contract-violations") >= 0, "contract-violations present");
        CHECK(hash_int(cs, "dispatch-hits") >= 0, "dispatch-hits present");
        CHECK(hash_int(cs, "dispatch-misses") >= 0, "dispatch-misses present");
        CHECK(hash_int(cs, "zerooverhead-wins") >= 0, "zerooverhead-wins present");
        CHECK(hash_int(cs, "zerooverhead-rate-pct") >= 0, "zerooverhead-rate-pct present");
        CHECK(hash_int(cs, "passes-skipped-dirty") >= 0, "passes-skipped-dirty present");
        CHECK(hash_int(cs, "pass-pipeline-runs") >= 0, "pass-pipeline-runs present");
        CHECK(hash_int(cs, "relower-skipped") >= 0, "relower-skipped present");
        CHECK(hash_int(cs, "mark-dirty-upward-calls") >= 0, "mark-dirty-upward-calls present");
        CHECK(hash_int(cs, "dirty-propagation") >= 0, "dirty-propagation present");
        CHECK(hash_int(cs, "contracts-coverage-pct") >= 0, "contracts-coverage-pct present");
        CHECK(hash_int(cs, "contracts-production-hotpath-total") >= 0,
              "contracts-production-hotpath-total present");
        CHECK(hash_int(cs, "contracts-production-hotpath-recommendation") >= 0,
              "contracts-production-hotpath-recommendation present");
    }

    const auto dirty_before = hash_int(cs, "mark-dirty-upward-calls");
    const auto total_before = hash_int(cs, "contracts-production-hotpath-total");

    // AC2: Guard mutate bumps dirty propagation observability
    {
        std::println("\n--- AC2: mutate:rebind dirty propagation ---");
        CHECK(cs.eval("(mutate:rebind \"f\" \"(lambda (x) (+ x 2))\" \"issue535\")").has_value(),
              "mutate:rebind lambda under Guard");
        (void)cs.eval("(eval-current)");
        const auto dirty_after = hash_int(cs, "mark-dirty-upward-calls");
        CHECK(dirty_after >= dirty_before,
              std::format("mark-dirty-upward-calls non-decreasing ({} -> {})", dirty_before,
                          dirty_after));
    }

    // AC3: second mutate + query cycle
    {
        std::println("\n--- AC3: mutate batch ---");
        CHECK(cs.eval("(mutate:rebind \"base\" \"20\")").has_value(), "second mutate under Guard");
        (void)cs.eval("(query:pattern \"f\")");
        const auto total_after = hash_int(cs, "contracts-production-hotpath-total");
        CHECK(total_after >= total_before,
              std::format("contracts-production-hotpath-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related contracts primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto task4 = cs.eval("(stats:get \"query:task4-hotpath-contracts\")");
        auto pass_contracts = cs.eval("(engine:metrics \"query:pass-contracts-stats\")");
        auto hotpath_hash = cs.eval("(engine:metrics \"query:contracts-hotpath-stats-hash\")");
        CHECK(task4 && aura::compiler::types::is_hash(*task4),
              "query:task4-hotpath-contracts hash regression");
        CHECK(pass_contracts && aura::compiler::types::is_int(*pass_contracts),
              "query:pass-contracts-stats int regression");
        CHECK(hotpath_hash && aura::compiler::types::is_hash(*hotpath_hash),
              "query:contracts-hotpath-stats-hash hash regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 130,
              "stats:count >= 130");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_535_observability_run();
}
#endif
