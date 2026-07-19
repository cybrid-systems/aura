// @category: integration
// @reason: Issue #572 — pass-pipeline-dirtyaware-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_572_detail {
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
        "(hash-ref (engine:metrics \"query:pass-pipeline-dirtyaware-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (add1 x) (+ x 1)) (define (dbl y) (* y 2)) "
                 "(define base 10) (define acc 0) (add1 1) (dbl 3)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_572_detail

int main() {
    using namespace aura_issue_572_detail;

    std::println("=== Issue #572: pass-pipeline-dirtyaware-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup + eval-current");

    // AC1: query:pass-pipeline-dirtyaware-stats returns hash
    {
        std::println("\n--- AC1: query:pass-pipeline-dirtyaware-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:pass-pipeline-dirtyaware-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:pass-pipeline-dirtyaware-stats returns hash");
        CHECK(hash_int(cs, "pass-pipeline-runs") >= 0, "pass-pipeline-runs present");
        CHECK(hash_int(cs, "pipeline-yield-count") >= 0, "pipeline-yield-count present");
        CHECK(hash_int(cs, "passes-skipped-due-to-dirty") >= 0,
              "passes-skipped-due-to-dirty present");
        CHECK(hash_int(cs, "passes-skipped-dirty-pipeline") >= 0,
              "passes-skipped-dirty-pipeline present");
        CHECK(hash_int(cs, "passes-skipped-type-dirty") >= 0, "passes-skipped-type-dirty present");
        CHECK(hash_int(cs, "wrap-delegation-count") >= 0, "wrap-delegation-count present");
        CHECK(hash_int(cs, "relower-skipped") >= 0, "relower-skipped present");
        CHECK(hash_int(cs, "relower-per-fn") >= 0, "relower-per-fn present");
        CHECK(hash_int(cs, "module-dirty-skips") >= 0, "module-dirty-skips present");
        CHECK(hash_int(cs, "ir-soa-block-dirty-hits") >= 0, "ir-soa-block-dirty-hits present");
        CHECK(hash_int(cs, "incremental-latency-win-pct") >= 0,
              "incremental-latency-win-pct present");
        CHECK(hash_int(cs, "task4-review-schema") == 572, "task4-review-schema == 572");
        CHECK(hash_int(cs, "pass-pipeline-dirtyaware-total") >= 0,
              "pass-pipeline-dirtyaware-total present");
        CHECK(hash_int(cs, "pass-pipeline-dirtyaware-recommendation") >= 0,
              "pass-pipeline-dirtyaware-recommendation present");
    }

    const auto total_before = hash_int(cs, "pass-pipeline-dirtyaware-total");
    const auto relower_before = hash_int(cs, "relower-per-fn");
    const auto skip_before = hash_int(cs, "passes-skipped-type-dirty");

    // AC2: mutate + eval grows pipeline counters
    {
        std::println("\n--- AC2: pipeline counters grow under workload ---");
        CHECK(cs.eval("(eval-current)").has_value(), "re-eval current");
        const auto mr = cs.typed_mutate("(mutate:rebind \"base\" \"99\")");
        CHECK(mr.success, "typed_mutate rebind");
        CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
        const auto total_after = hash_int(cs, "pass-pipeline-dirtyaware-total");
        const auto relower_after = hash_int(cs, "relower-per-fn");
        const auto skip_after = hash_int(cs, "passes-skipped-type-dirty");
        CHECK(total_after >= total_before,
              std::format("pass-pipeline-dirtyaware-total monotonic ({} -> {})", total_before,
                          total_after));
        CHECK(relower_after >= relower_before,
              std::format("relower-per-fn monotonic ({} -> {})", relower_before, relower_after));
        CHECK(
            skip_after >= skip_before,
            std::format("passes-skipped-type-dirty monotonic ({} -> {})", skip_before, skip_after));
    }

    // AC3: second mutate + query cycle
    {
        std::println("\n--- AC3: second mutate + query cycle ---");
        CHECK(cs.eval("(mutate:rebind \"acc\" \"5\")").has_value(), "second mutate under Guard");
        (void)cs.eval("(query:pattern \"add1\")");
        const auto total_after = hash_int(cs, "pass-pipeline-dirtyaware-total");
        CHECK(total_after >= total_before,
              std::format("pass-pipeline-dirtyaware-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related pass pipeline primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto pipeline = cs.eval("(engine:metrics \"query:pass-pipeline-stats\")");
        auto contracts = cs.eval("(engine:metrics \"query:pass-contracts-stats\")");
        auto incremental =
            cs.eval("(engine:metrics \"query:pass-pipeline-incremental-stats-hash\")");
        CHECK(pipeline && aura::compiler::types::is_hash(*pipeline),
              "query:pass-pipeline-stats hash regression (#494)");
        CHECK(contracts && aura::compiler::types::is_int(*contracts),
              "query:pass-contracts-stats int regression (#406)");
        CHECK(incremental && aura::compiler::types::is_hash(*incremental),
              "query:pass-pipeline-incremental-stats-hash regression (#625)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 139,
              "stats:count >= 139");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}