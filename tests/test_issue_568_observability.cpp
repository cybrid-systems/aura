// @category: integration
// @reason: Issue #568 — soa-children-columnar-migration-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_568_detail {
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
        "(hash-ref (engine:metrics \"query:soa-children-columnar-migration-stats\") '{}')", key));
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

} // namespace aura_issue_568_detail

int main() {
    using namespace aura_issue_568_detail;

    std::println("=== Issue #568: soa-children-columnar-migration-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "recursive define workspace setup");

    // AC1: query:soa-children-columnar-migration-stats returns hash
    {
        std::println("\n--- AC1: query:soa-children-columnar-migration-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:soa-children-columnar-migration-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:soa-children-columnar-migration-stats returns hash");
        CHECK(hash_int(cs, "children-call-count") >= 0, "children-call-count present");
        CHECK(hash_int(cs, "children-safe-view-count") >= 0, "children-safe-view-count present");
        CHECK(hash_int(cs, "child-columnar-hit-rate-pct") >= 0,
              "child-columnar-hit-rate-pct present");
        CHECK(hash_int(cs, "soa-functions-visited") >= 0, "soa-functions-visited present");
        CHECK(hash_int(cs, "soa-instructions-visited") >= 0, "soa-instructions-visited present");
        CHECK(hash_int(cs, "ir-soa-view-cache-hits") >= 0, "ir-soa-view-cache-hits present");
        CHECK(hash_int(cs, "irsoa-wired-hits") >= 0, "irsoa-wired-hits present");
        CHECK(hash_int(cs, "ir-soa-instr-emitted") >= 0, "ir-soa-instr-emitted present");
        CHECK(hash_int(cs, "ir-soa-func-emitted") >= 0, "ir-soa-func-emitted present");
        CHECK(hash_int(cs, "passes-skipped-due-to-dirty") >= 0,
              "passes-skipped-due-to-dirty present");
        CHECK(hash_int(cs, "relower-block-count") >= 0, "relower-block-count present");
        CHECK(hash_int(cs, "blocks-saved") >= 0, "blocks-saved present");
        CHECK(hash_int(cs, "ir-soa-block-dirty-hits") >= 0, "ir-soa-block-dirty-hits present");
        CHECK(hash_int(cs, "migration-schema") == 568, "migration-schema == 568");
        CHECK(hash_int(cs, "soa-children-columnar-migration-total") >= 0,
              "soa-children-columnar-migration-total present");
        CHECK(hash_int(cs, "soa-children-columnar-migration-recommendation") >= 0,
              "soa-children-columnar-migration-recommendation present");
    }

    const auto relower_before = hash_int(cs, "relower-block-count");
    const auto total_before = hash_int(cs, "soa-children-columnar-migration-total");

    // AC2: mutate triggers dirty propagation + re-lower observability
    {
        std::println("\n--- AC2: mutate:rebind dirty propagation ---");
        CHECK(cs.eval("(mutate:rebind \"a\" \"10\")").has_value(), "mutate:rebind under Guard");
        (void)cs.eval("(eval-current)");
        const auto relower_after = hash_int(cs, "relower-block-count");
        CHECK(relower_after >= relower_before,
              std::format("relower-block-count non-decreasing ({} -> {})", relower_before,
                          relower_after));
        CHECK(hash_int(cs, "irsoa-wired-hits") >= 0, "irsoa-wired-hits readable after mutate");
    }

    // AC3: structural query:pattern columnar walk + second mutate cycle
    {
        std::println("\n--- AC3: query:pattern + mutate batch ---");
        const auto safe_before = hash_int(cs, "children-safe-view-count");
        (void)cs.eval("(query:pattern \"(define ?sym ?val)\")");
        const auto safe_after = hash_int(cs, "children-safe-view-count");
        CHECK(safe_after > safe_before,
              std::format("children-safe-view-count grew ({} -> {})", safe_before, safe_after));
        CHECK(cs.eval("(mutate:rebind \"b\" \"20\")").has_value(), "second mutate under Guard");
        (void)cs.eval("(query:pattern \"fact\")");
        const auto total_after = hash_int(cs, "soa-children-columnar-migration-total");
        CHECK(total_after >= total_before,
              std::format("soa-children-columnar-migration-total monotonic ({} -> {})",
                          total_before, total_after));
    }

    // AC4: related SoA/columnar primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto columnar = cs.eval("(engine:metrics \"query:soa-production-columnar-stats\")");
        auto adoption = cs.eval("(engine:metrics \"query:soa-adoption-stats\")");
        auto hotpath = cs.eval("(engine:metrics \"query:soa-hotpath-adoption-stats\")");
        auto irsoa = cs.eval("(engine:metrics \"query:irsoa-incremental-stats\")");
        CHECK(columnar && aura::compiler::types::is_hash(*columnar),
              "query:soa-production-columnar-stats hash regression (#533)");
        CHECK(adoption && aura::compiler::types::is_hash(*adoption),
              "query:soa-adoption-stats hash regression (#463)");
        CHECK(hotpath && aura::compiler::types::is_int(*hotpath),
              "query:soa-hotpath-adoption-stats int regression (#506)");
        CHECK(irsoa && aura::compiler::types::is_hash(*irsoa),
              "query:irsoa-incremental-stats hash regression (#684)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 137,
              "stats:count >= 137");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}