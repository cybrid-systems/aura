// @category: integration
// @reason: Issue #494 — Pass pipeline JIT/incremental concepts + yield + stats hash

#include <format>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_494_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:pass-pipeline-stats) '{}')", key));
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

} // namespace aura_issue_494_detail

int main() {
    using namespace aura_issue_494_detail;

    std::println("=== Issue #494: Pass pipeline incremental/JIT stats ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup + eval-current");

    // AC1: query:pass-pipeline-stats fields
    {
        std::println("\n--- AC1: query:pass-pipeline-stats ---");
        auto stats = cs.eval("(query:pass-pipeline-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:pass-pipeline-stats returns hash");
        CHECK(snap_stat(cs, "pipeline-yield-count") >= 0, "pipeline-yield-count present");
        CHECK(snap_stat(cs, "passes-skipped-dirty") >= 0, "passes-skipped-dirty present");
        CHECK(snap_stat(cs, "passes-skipped-type-dirty") >= 0,
              "passes-skipped-type-dirty present");
        CHECK(snap_stat(cs, "relower-skipped") >= 0, "relower-skipped present");
        CHECK(snap_stat(cs, "relower-per-fn") >= 0, "relower-per-fn present");
        CHECK(snap_stat(cs, "module-dirty-skips") >= 0, "module-dirty-skips present");
        CHECK(snap_stat(cs, "pipeline-total") >= 0, "pipeline-total present");
    }

    const auto total_before = snap_stat(cs, "pipeline-total");
    const auto type_skip_before = snap_stat(cs, "passes-skipped-type-dirty");
    const auto relower_before = snap_stat(cs, "relower-per-fn");

    // AC2: eval + mutate grows pipeline counters
    {
        std::println("\n--- AC2: pipeline counters grow under workload ---");
        CHECK(cs.eval("(eval-current)").has_value(), "re-eval current");
        const auto mr = cs.typed_mutate("(mutate:rebind \"base\" \"99\")");
        CHECK(mr.success, "typed_mutate rebind");
        CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
        const auto total_after = snap_stat(cs, "pipeline-total");
        const auto type_skip_after = snap_stat(cs, "passes-skipped-type-dirty");
        const auto relower_after = snap_stat(cs, "relower-per-fn");
        CHECK(total_after >= total_before,
              std::format("pipeline-total monotonic ({} -> {})", total_before, total_after));
        CHECK(type_skip_after >= type_skip_before,
              std::format("passes-skipped-type-dirty monotonic ({} -> {})", type_skip_before,
                          type_skip_after));
        CHECK(relower_after >= relower_before,
              std::format("relower-per-fn monotonic ({} -> {})", relower_before, relower_after));
    }

    // AC3: query:pass-contracts-stats regression
    {
        std::println("\n--- AC3: pass-contracts-stats regression ---");
        auto contracts = cs.eval("(query:pass-contracts-stats)");
        CHECK(contracts && aura::compiler::types::is_int(*contracts),
              "query:pass-contracts-stats regression");
    }

    // AC4: stats:count
    {
        std::println("\n--- AC4: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) == 92,
              "stats:count == 92");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}