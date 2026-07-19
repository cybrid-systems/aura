// @category: integration
// @reason: Issue #491 — JIT opcode coverage + hot-swap safety + stats hash

#include <format>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_491_detail {
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

static std::int64_t snap_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"query:jit-stats-hash\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (f x) (+ x 1)) (define base 10) (f base)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_491_detail

int aura_issue_491_run() {
    using namespace aura_issue_491_detail;

    std::println("=== Issue #491: JIT production readiness + hot-swap safety ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup + eval-current");

    // AC1: query:jit-stats-hash fields
    {
        std::println("\n--- AC1: query:jit-stats-hash ---");
        auto stats = cs.eval("(engine:metrics \"query:jit-stats-hash\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats), "query:jit-stats-hash returns hash");
        CHECK(snap_stat(cs, "compiles") >= 0, "compiles present");
        CHECK(snap_stat(cs, "fallback-count") >= 0, "fallback-count present");
        CHECK(snap_stat(cs, "consistency-violations") >= 0, "consistency-violations present");
        CHECK(snap_stat(cs, "opcode-total") == 53, "opcode-total == 53");
        CHECK(snap_stat(cs, "opcode-coverage-pct") >= 0, "opcode-coverage-pct present");
        CHECK(snap_stat(cs, "hotswap-invalidate-total") >= 0, "hotswap-invalidate-total present");
        CHECK(snap_stat(cs, "epoch-mismatch-hits") >= 0, "epoch-mismatch-hits present");
    }

    const auto hotswap_before = snap_stat(cs, "hotswap-invalidate-total");
    const auto invalidate_before = snap_stat(cs, "invalidate-function-calls");

    // AC2: query:jit-stats string includes new metrics keys
    {
        std::println("\n--- AC2: query:jit-stats extended format ---");
        auto line = cs.eval("(engine:metrics \"query:jit-stats\")");
        CHECK(line && aura::compiler::types::is_string(*line), "query:jit-stats returns string");
        if (line && aura::compiler::types::is_string(*line)) {
            const auto idx = aura::compiler::types::as_string_idx(*line);
            const auto& s = cs.evaluator().string_heap()[idx];
            CHECK(s.find("fallback_count=") != std::string::npos,
                  "jit-stats includes fallback_count");
            CHECK(s.find("consistency_violations=") != std::string::npos,
                  "jit-stats includes consistency_violations");
        }
    }

    // AC3: post-invalidate hot-swap safety counters grow (lambda rebind
    // triggers invalidate_function; literal define mutate does not).
    {
        std::println("\n--- AC3: hot-swap safety on mutate ---");
        CHECK(cs.eval("(mutate:rebind \"f\" \"(lambda (x) (+ x 2))\" \"issue491\")").has_value(),
              "mutate:rebind lambda under Guard");
        const auto hotswap_after = snap_stat(cs, "hotswap-invalidate-total");
        const auto invalidate_after = snap_stat(cs, "invalidate-function-calls");
        CHECK(hotswap_after > hotswap_before,
              std::format("hotswap-invalidate grew ({} -> {})", hotswap_before, hotswap_after));
        CHECK(invalidate_after > invalidate_before,
              std::format("invalidate-function-calls grew ({} -> {})", invalidate_before,
                          invalidate_after));
        CHECK(cs.eval("(eval-current)").has_value(), "eval-current after mutate (parity smoke)");
    }

    // AC4: JIT fallback stats regression
    {
        std::println("\n--- AC4: query:jit-fallback-stats regression ---");
        auto fb = cs.eval("(engine:metrics \"query:jit-fallback-stats\")");
        auto jit = cs.eval("(engine:metrics \"query:jit-stats\")");
        CHECK(fb && aura::compiler::types::is_int(*fb), "query:jit-fallback-stats regression");
        CHECK(jit && aura::compiler::types::is_string(*jit), "query:jit-stats regression");
    }

    // AC5: mutate-then-query closed loop
    {
        std::println("\n--- AC5: mutate-then-query closed loop ---");
        CHECK(cs.eval("(query:pattern \"f\")").has_value(), "query:pattern after mutate");
        CHECK(snap_stat(cs, "opcode-coverage-pct") >= 0, "coverage still observable");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        // Loose assertion: count >= 89 baseline. #601 added
        // (engine:metrics \"query:jit-hotswap-closure-stats\") and brought it to 90+;
        // future issues will continue to bump it. Don't pin to a
        // brittle exact value here.
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 89,
              "stats:count >= 89 (#491 baseline)");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_491_run();
}
#endif
