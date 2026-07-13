// @category: integration
// @reason: Issue #532 — jit-consistency-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_532_detail {
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
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:jit-consistency-stats\") '{}')", key));
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

} // namespace aura_issue_532_detail

int aura_issue_532_observability_run() {
    using namespace aura_issue_532_detail;

    std::println("=== Issue #532: jit-consistency-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup + eval-current");

    // AC1: query:jit-consistency-stats returns hash
    {
        std::println("\n--- AC1: query:jit-consistency-stats ---");
        auto stats = cs.eval("(query:jit-consistency-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:jit-consistency-stats returns hash");
        CHECK(hash_int(cs, "unhandled-count") >= 0, "unhandled-count present");
        CHECK(hash_int(cs, "fallback-count") >= 0, "fallback-count present");
        CHECK(hash_int(cs, "consistency-violations") >= 0, "consistency-violations present");
        CHECK(hash_int(cs, "compiles") >= 0, "compiles present");
        CHECK(hash_int(cs, "opcode-coverage-pct") >= 0, "opcode-coverage-pct present");
        CHECK(hash_int(cs, "deopt-count") >= 0, "deopt-count present");
        CHECK(hash_int(cs, "deopt-rate-pct") >= 0, "deopt-rate-pct present");
        CHECK(hash_int(cs, "hotswap-invalidate-count") >= 0, "hotswap-invalidate-count present");
        CHECK(hash_int(cs, "live-closure-refreshed") >= 0, "live-closure-refreshed present");
        CHECK(hash_int(cs, "forced-deopt-total") >= 0, "forced-deopt-total present");
        CHECK(hash_int(cs, "hotswap-success-rate-pct") >= 0, "hotswap-success-rate-pct present");
        CHECK(hash_int(cs, "linear-check-hits") >= 0, "linear-check-hits present");
        CHECK(hash_int(cs, "bridge-epoch-hits") >= 0, "bridge-epoch-hits present");
        CHECK(hash_int(cs, "epoch-mismatch-hits") >= 0, "epoch-mismatch-hits present");
        CHECK(hash_int(cs, "safe-fallbacks") >= 0, "safe-fallbacks present");
        CHECK(hash_int(cs, "jit-consistency-total") >= 0, "jit-consistency-total present");
        CHECK(hash_int(cs, "jit-consistency-recommendation") >= 0,
              "jit-consistency-recommendation present");
    }

    const auto hotswap_before = hash_int(cs, "hotswap-invalidate-count");
    const auto total_before = hash_int(cs, "jit-consistency-total");

    // AC2: lambda mutate bumps hot-swap safety counters
    {
        std::println("\n--- AC2: hot-swap safety on mutate ---");
        CHECK(cs.eval("(mutate:rebind \"f\" \"(lambda (x) (+ x 2))\" \"issue532\")").has_value(),
              "mutate:rebind lambda under Guard");
        (void)cs.eval("(eval-current)");
        const auto hotswap_after = hash_int(cs, "hotswap-invalidate-count");
        CHECK(hotswap_after > hotswap_before,
              std::format("hotswap-invalidate grew ({} -> {})", hotswap_before, hotswap_after));
    }

    // AC3: second mutate + query cycle
    {
        std::println("\n--- AC3: mutate batch ---");
        CHECK(cs.eval("(mutate:rebind \"base\" \"20\")").has_value(), "second mutate under Guard");
        (void)cs.eval("(query:pattern \"f\")");
        const auto total_after = hash_int(cs, "jit-consistency-total");
        CHECK(total_after >= total_before,
              std::format("jit-consistency-total monotonic ({} -> {})", total_before, total_after));
    }

    // AC4: related JIT primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto jit_hash = cs.eval("(query:jit-stats-hash)");
        auto hotswap = cs.eval("(query:jit-hotswap-closure-stats)");
        auto jit_line = cs.eval("(query:jit-stats)");
        CHECK(jit_hash && aura::compiler::types::is_hash(*jit_hash),
              "query:jit-stats-hash hash regression");
        CHECK(hotswap && aura::compiler::types::is_hash(*hotswap),
              "query:jit-hotswap-closure-stats hash regression");
        CHECK(jit_line && aura::compiler::types::is_string(*jit_line),
              "query:jit-stats string regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 127,
              "stats:count >= 127");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_532_observability_run();
}
#endif
