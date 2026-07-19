// @category: integration
// @reason: Issue #493 — EDSL hot-path bottleneck measurement + stats hash

#include <format>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_493_detail {
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
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:hotpath-bottleneck-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define (dbl y) (* y 2)) "
                 "(define base 10) (define acc 0) "
                 "(add1 1) (dbl 3)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_493_detail

int aura_issue_493_run() {
    using namespace aura_issue_493_detail;

    std::println("=== Issue #493: EDSL hot-path bottleneck stats ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup + eval-current");

    // AC1: query:hotpath-bottleneck-stats fields
    {
        std::println("\n--- AC1: query:hotpath-bottleneck-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:hotpath-bottleneck-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:hotpath-bottleneck-stats returns hash");
        CHECK(snap_stat(cs, "eval-flat-calls") >= 0, "eval-flat-calls present");
        CHECK(snap_stat(cs, "lowering-calls") >= 0, "lowering-calls present");
        CHECK(snap_stat(cs, "soa-dual-emit-hits") >= 0, "soa-dual-emit-hits present");
        CHECK(snap_stat(cs, "dirty-upward-calls") >= 0, "dirty-upward-calls present");
        CHECK(snap_stat(cs, "dirty-early-exit") >= 0, "dirty-early-exit present");
        CHECK(snap_stat(cs, "passes-skipped-dirty") >= 0, "passes-skipped-dirty present");
        CHECK(snap_stat(cs, "bottleneck-total") >= 0, "bottleneck-total present");
    }

    const auto eval_flat_before = snap_stat(cs, "eval-flat-calls");
    const auto lowering_before = snap_stat(cs, "lowering-calls");
    const auto total_before = snap_stat(cs, "bottleneck-total");

    // AC2: eval + mutate grows hot-path counters
    {
        std::println("\n--- AC2: hot-path counters grow under workload ---");
        CHECK(cs.eval("(eval-current)").has_value(), "re-eval current");
        const auto eval_flat_after = snap_stat(cs, "eval-flat-calls");
        const auto lowering_after = snap_stat(cs, "lowering-calls");
        CHECK(eval_flat_after > eval_flat_before,
              std::format("eval-flat-calls grew ({} -> {})", eval_flat_before, eval_flat_after));
        CHECK(lowering_after >= lowering_before,
              std::format("lowering-calls monotonic ({} -> {})", lowering_before, lowering_after));
        const auto mr = cs.typed_mutate("(mutate:rebind \"base\" \"99\")");
        CHECK(mr.success, "typed_mutate rebind");
        CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
        const auto total_after = snap_stat(cs, "bottleneck-total");
        CHECK(total_after > total_before,
              std::format("bottleneck-total grew ({} -> {})", total_before, total_after));
    }

    // AC3: query:soa-hotpath-adoption-stats regression
    {
        std::println("\n--- AC3: soa-hotpath-adoption-stats regression ---");
        auto soa = cs.eval("(engine:metrics \"query:soa-hotpath-adoption-stats\")");
        CHECK(soa && aura::compiler::types::is_int(*soa), "soa-hotpath-adoption-stats regression");
    }

    // AC4: query:dirty-propagation-cost-stats regression
    {
        std::println("\n--- AC4: dirty-propagation-cost-stats regression ---");
        auto dirty = cs.eval("(engine:metrics \"query:dirty-propagation-cost-stats\")");
        CHECK(dirty && aura::compiler::types::is_int(*dirty),
              "dirty-propagation-cost-stats regression");
    }

    // AC5: mutate-then-query closed loop
    {
        std::println("\n--- AC5: mutate-then-query closed loop ---");
        CHECK(cs.eval("(query:pattern \"add1\")").has_value(), "query:pattern after mutate");
        CHECK(snap_stat(cs, "soa-instr-emitted") >= 0, "soa-instr-emitted observable");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 211,
              "stats:count >= 211");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_493_run();
}
#endif
