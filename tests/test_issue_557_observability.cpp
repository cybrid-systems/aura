// @category: integration
// @reason: Issue #557 — top5-commercial-coverage-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_557_detail {
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
        "(hash-ref (engine:metrics \"query:top5-commercial-coverage-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_closure_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) (fact 3)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_557_detail

int aura_issue_557_observability_run() {
    using namespace aura_issue_557_detail;

    std::println("=== Issue #557: top5-commercial-coverage-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_closure_workspace(cs), "recursive closure workspace setup");

    // AC1: query:top5-commercial-coverage-stats returns hash
    {
        std::println("\n--- AC1: query:top5-commercial-coverage-stats ---");
        auto stats = cs.eval("(query:top5-commercial-coverage-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:top5-commercial-coverage-stats returns hash");
        CHECK(hash_int(cs, "closure-stale-refresh") >= 0, "closure-stale-refresh present");
        CHECK(hash_int(cs, "bridge-epoch-hits") >= 0, "bridge-epoch-hits present");
        CHECK(hash_int(cs, "linear-check-pass") >= 0, "linear-check-pass present");
        CHECK(hash_int(cs, "env-stale-refresh") >= 0, "env-stale-refresh present");
        CHECK(hash_int(cs, "blocks-saved") >= 0, "blocks-saved present");
        CHECK(hash_int(cs, "partial-relowers") >= 0, "partial-relowers present");
        CHECK(hash_int(cs, "invalidate-function-calls") >= 0, "invalidate-function-calls present");
        CHECK(hash_int(cs, "min-scope-hit-rate-pct") >= 0, "min-scope-hit-rate-pct present");
        CHECK(hash_int(cs, "unhandled-opcode-count") >= 0, "unhandled-opcode-count present");
        CHECK(hash_int(cs, "deopt-count") >= 0, "deopt-count present");
        CHECK(hash_int(cs, "hotswap-invalidate-count") >= 0, "hotswap-invalidate-count present");
        CHECK(hash_int(cs, "opcode-coverage-pct") >= 0, "opcode-coverage-pct present");
        CHECK(hash_int(cs, "steal-attempts") >= 0, "steal-attempts present");
        CHECK(hash_int(cs, "boundary-violations") >= 0, "boundary-violations present");
        CHECK(hash_int(cs, "unsafe-boundary-attempts") >= 0, "unsafe-boundary-attempts present");
        CHECK(hash_int(cs, "lock-contention-us") >= 0, "lock-contention-us present");
        CHECK(hash_int(cs, "batch-commits") >= 0, "batch-commits present");
        CHECK(hash_int(cs, "batch-rollbacks") >= 0, "batch-rollbacks present");
        CHECK(hash_int(cs, "bumps-saved") >= 0, "bumps-saved present");
        CHECK(hash_int(cs, "steal-violations-during-batch") >= 0,
              "steal-violations-during-batch present");
        CHECK(hash_int(cs, "top5-commercial-coverage-total") >= 0,
              "top5-commercial-coverage-total present");
        CHECK(hash_int(cs, "top5-commercial-coverage-recommendation") >= 0,
              "top5-commercial-coverage-recommendation present");
    }

    const auto hotswap_before = hash_int(cs, "hotswap-invalidate-count");
    const auto partial_before = hash_int(cs, "partial-relowers");
    const auto total_before = hash_int(cs, "top5-commercial-coverage-total");

    // AC2: closure mutate + eval bumps incremental/JIT counters
    {
        std::println("\n--- AC2: mutate:rebind closure body ---");
        CHECK(
            cs.eval("(mutate:rebind \"fact\" \"(lambda (n) (if (= n 0) 1 (* n (fact (- n 1)))))\" "
                    "\"issue557\")")
                .has_value(),
            "mutate:rebind recursive closure under Guard");
        (void)cs.eval("(eval-current)");
        const auto hotswap_after = hash_int(cs, "hotswap-invalidate-count");
        const auto partial_after = hash_int(cs, "partial-relowers");
        CHECK(hotswap_after > hotswap_before,
              std::format("hotswap-invalidate grew ({} -> {})", hotswap_before, hotswap_after));
        CHECK(partial_after >= partial_before,
              std::format("partial-relowers non-decreasing ({} -> {})", partial_before,
                          partial_after));
    }

    // AC3: second mutate + query cycle
    {
        std::println("\n--- AC3: mutate batch ---");
        CHECK(
            cs.eval("(mutate:rebind \"fact\" \"(lambda (n) (+ n 1))\" \"issue557b\")").has_value(),
            "second mutate under Guard");
        (void)cs.eval("(query:pattern \"fact\")");
        const auto total_after = hash_int(cs, "top5-commercial-coverage-total");
        CHECK(total_after >= total_before,
              std::format("top5-commercial-coverage-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related Top 5 pillar primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto closure = cs.eval("(query:closure-env-safety-stats)");
        auto incr = cs.eval("(query:incremental-production-relower-stats)");
        auto jit = cs.eval("(query:jit-consistency-stats)");
        auto edsl = cs.eval("(query:edsl-concurrency-stats)");
        auto batch = cs.eval("(query:mutation-log-stats)");
        CHECK(closure && aura::compiler::types::is_hash(*closure),
              "query:closure-env-safety-stats hash regression (#531)");
        CHECK(incr && aura::compiler::types::is_hash(*incr),
              "query:incremental-production-relower-stats hash regression (#530)");
        CHECK(jit && aura::compiler::types::is_hash(*jit),
              "query:jit-consistency-stats hash regression (#532)");
        CHECK(edsl && aura::compiler::types::is_int(*edsl),
              "query:edsl-concurrency-stats int regression (#556)");
        CHECK(batch && aura::compiler::types::is_int(*batch),
              "query:mutation-log-stats int regression (#553)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 135,
              "stats:count >= 135");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_557_observability_run();
}
#endif
