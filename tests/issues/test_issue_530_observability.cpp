// @category: integration
// @reason: Issue #530 — incremental-production-relower-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_530_detail {
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
        "(hash-ref (engine:metrics \"query:incremental-production-relower-stats\") '{}')", key));
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

} // namespace aura_issue_530_detail

int aura_issue_530_observability_run() {
    using namespace aura_issue_530_detail;

    std::println("=== Issue #530: incremental-production-relower-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "recursive define workspace setup");

    // AC1: query:incremental-production-relower-stats returns hash
    {
        std::println("\n--- AC1: query:incremental-production-relower-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:incremental-production-relower-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:incremental-production-relower-stats returns hash");
        CHECK(hash_int(cs, "should-relower-triggers") >= 0, "should-relower-triggers present");
        CHECK(hash_int(cs, "partial-relowers") >= 0, "partial-relowers present");
        CHECK(hash_int(cs, "impact-scope-calls") >= 0, "impact-scope-calls present");
        CHECK(hash_int(cs, "affected-blocks-total") >= 0, "affected-blocks-total present");
        CHECK(hash_int(cs, "relower-skipped") >= 0, "relower-skipped present");
        CHECK(hash_int(cs, "relower-per-fn") >= 0, "relower-per-fn present");
        CHECK(hash_int(cs, "relower-full") >= 0, "relower-full present");
        CHECK(hash_int(cs, "blocks-saved") >= 0, "blocks-saved present");
        CHECK(hash_int(cs, "jit-invalidate-count") >= 0, "jit-invalidate-count present");
        CHECK(hash_int(cs, "bridge-invalidate-count") >= 0, "bridge-invalidate-count present");
        CHECK(hash_int(cs, "invalidate-function-calls") >= 0, "invalidate-function-calls present");
        CHECK(hash_int(cs, "cascade-body-only") >= 0, "cascade-body-only present");
        CHECK(hash_int(cs, "dirty-blocks") >= 0, "dirty-blocks present");
        CHECK(hash_int(cs, "dirty-functions") >= 0, "dirty-functions present");
        CHECK(hash_int(cs, "cached-functions") >= 0, "cached-functions present");
        CHECK(hash_int(cs, "dirty-block-pct") >= 0, "dirty-block-pct present");
        CHECK(hash_int(cs, "min-scope-hit-rate-pct") >= 0, "min-scope-hit-rate-pct present");
        CHECK(hash_int(cs, "incremental-production-relower-total") >= 0,
              "incremental-production-relower-total present");
        CHECK(hash_int(cs, "incremental-production-relower-recommendation") >= 0,
              "incremental-production-relower-recommendation present");
    }

    const auto total_before = hash_int(cs, "incremental-production-relower-total");
    const auto partial_before = cs.evaluator().get_partial_relower_count();

    // AC2: mutate triggers incremental re-lower observability
    {
        std::println("\n--- AC2: mutate:rebind incremental path ---");
        CHECK(cs.eval("(mutate:rebind \"a\" \"10\")").has_value(), "mutate:rebind under Guard");
        (void)cs.eval("(eval-current)");
        const auto partial_after = cs.evaluator().get_partial_relower_count();
        CHECK(partial_after >= partial_before,
              std::format("partial_relower non-decreasing ({} -> {})", partial_before,
                          partial_after));
        CHECK(hash_int(cs, "cached-functions") >= 0, "cached-functions readable after mutate");
    }

    // AC3: second mutate + query cycle
    {
        std::println("\n--- AC3: mutate batch ---");
        CHECK(cs.eval("(mutate:rebind \"b\" \"20\")").has_value(), "second mutate under Guard");
        (void)cs.eval("(query:pattern \"fact\")");
        const auto total_after = hash_int(cs, "incremental-production-relower-total");
        CHECK(total_after >= total_before,
              std::format("incremental-production-relower-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related incremental primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto cis = cs.eval("(engine:metrics \"query:compiler-incremental-stats\")");
        auto iis = cs.eval("(engine:metrics \"query:ir-soa-incremental-stats\")");
        auto cache = cs.eval("(engine:metrics \"query:compiler-cache-stats\")");
        CHECK(cis && aura::compiler::types::is_int(*cis),
              "compiler-incremental-stats int regression");
        CHECK(iis && aura::compiler::types::is_int(*iis),
              "ir-soa-incremental-stats int regression");
        CHECK(cache && aura::compiler::types::is_pair(*cache),
              "compiler-cache-stats tuple regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 126,
              "stats:count >= 126");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_530_observability_run();
}
#endif
