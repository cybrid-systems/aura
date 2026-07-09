// @category: integration
// @reason: Issue #533 — soa-production-columnar-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_533_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:soa-production-columnar-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) "
                 "(define a 1) (define b 2) (fact 3)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_533_detail

int aura_issue_533_observability_run() {
    using namespace aura_issue_533_detail;

    std::println("=== Issue #533: soa-production-columnar-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "recursive define workspace setup");

    // AC1: query:soa-production-columnar-stats returns hash
    {
        std::println("\n--- AC1: query:soa-production-columnar-stats ---");
        auto stats = cs.eval("(query:soa-production-columnar-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:soa-production-columnar-stats returns hash");
        CHECK(hash_int(cs, "children-call-count") >= 0, "children-call-count present");
        CHECK(hash_int(cs, "children-safe-view-count") >= 0, "children-safe-view-count present");
        CHECK(hash_int(cs, "mark-dirty-upward-calls") >= 0, "mark-dirty-upward-calls present");
        CHECK(hash_int(cs, "mark-dirty-total-nodes") >= 0, "mark-dirty-total-nodes present");
        CHECK(hash_int(cs, "dirty-fast-fixed-point-hits") >= 0,
              "dirty-fast-fixed-point-hits present");
        CHECK(hash_int(cs, "soa-functions-visited") >= 0, "soa-functions-visited present");
        CHECK(hash_int(cs, "soa-instructions-visited") >= 0, "soa-instructions-visited present");
        CHECK(hash_int(cs, "aos-view-built-count") >= 0, "aos-view-built-count present");
        CHECK(hash_int(cs, "ir-soa-view-cache-hits") >= 0, "ir-soa-view-cache-hits present");
        CHECK(hash_int(cs, "irsoa-wired-hits") >= 0, "irsoa-wired-hits present");
        CHECK(hash_int(cs, "ir-soa-block-dirty-hits") >= 0, "ir-soa-block-dirty-hits present");
        CHECK(hash_int(cs, "blocks-saved") >= 0, "blocks-saved present");
        CHECK(hash_int(cs, "passes-skipped-type-dirty") >= 0, "passes-skipped-type-dirty present");
        CHECK(hash_int(cs, "passes-skipped-dirty-pipeline") >= 0,
              "passes-skipped-dirty-pipeline present");
        CHECK(hash_int(cs, "ir-soa-instr-emitted") >= 0, "ir-soa-instr-emitted present");
        CHECK(hash_int(cs, "ir-soa-func-emitted") >= 0, "ir-soa-func-emitted present");
        CHECK(hash_int(cs, "dirty-skip-rate-pct") >= 0, "dirty-skip-rate-pct present");
        CHECK(hash_int(cs, "columnar-locality-pct") >= 0, "columnar-locality-pct present");
        CHECK(hash_int(cs, "soa-production-columnar-total") >= 0,
              "soa-production-columnar-total present");
        CHECK(hash_int(cs, "soa-production-columnar-recommendation") >= 0,
              "soa-production-columnar-recommendation present");
    }

    const auto dirty_up_before = hash_int(cs, "mark-dirty-upward-calls");
    const auto total_before = hash_int(cs, "soa-production-columnar-total");

    // AC2: mutate triggers dirty propagation observability
    {
        std::println("\n--- AC2: mutate:rebind dirty propagation ---");
        CHECK(cs.eval("(mutate:rebind \"a\" \"10\")").has_value(), "mutate:rebind under Guard");
        (void)cs.eval("(eval-current)");
        const auto dirty_up_after = hash_int(cs, "mark-dirty-upward-calls");
        CHECK(dirty_up_after >= dirty_up_before,
              std::format("mark-dirty-upward-calls non-decreasing ({} -> {})", dirty_up_before,
                          dirty_up_after));
        CHECK(hash_int(cs, "irsoa-wired-hits") >= 0, "irsoa-wired-hits readable after mutate");
    }

    // AC3: second mutate + query cycle
    {
        std::println("\n--- AC3: mutate batch ---");
        CHECK(cs.eval("(mutate:rebind \"b\" \"20\")").has_value(), "second mutate under Guard");
        (void)cs.eval("(query:pattern \"fact\")");
        const auto total_after = hash_int(cs, "soa-production-columnar-total");
        CHECK(total_after >= total_before,
              std::format("soa-production-columnar-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related SoA primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto adoption = cs.eval("(query:soa-adoption-stats)");
        auto hotpath = cs.eval("(query:soa-hotpath-adoption-stats)");
        auto incremental = cs.eval("(query:ir-soa-incremental-stats)");
        CHECK(adoption && aura::compiler::types::is_hash(*adoption),
              "query:soa-adoption-stats hash regression");
        CHECK(hotpath && aura::compiler::types::is_int(*hotpath),
              "query:soa-hotpath-adoption-stats int regression");
        CHECK(incremental && aura::compiler::types::is_int(*incremental),
              "query:ir-soa-incremental-stats int regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 128,
              "stats:count >= 128");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_533_observability_run();
}
#endif
