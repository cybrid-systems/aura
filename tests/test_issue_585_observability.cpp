// @category: integration
// @reason: Issue #585 — primitives-error-stats unified recovery slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_585_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:primitives-error-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define acc 0)\")")) {
        return false;
    }
    if (!cs.eval("(eval-current)").has_value()) {
        return false;
    }
    return cs.eval("(define xs (cons 1 (cons 2 (cons 3 ()))))").has_value();
}

} // namespace aura_issue_585_detail

int main() {
    using namespace aura_issue_585_detail;

    std::println("=== Issue #585: primitives-error-stats unified recovery ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup");

    // AC1: query:primitives-error-stats returns hash with #585 fields
    {
        std::println("\n--- AC1: query:primitives-error-stats ---");
        auto stats = cs.eval("(query:primitives-error-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:primitives-error-stats returns hash");
        CHECK(hash_int(cs, "primitive-error-count") >= 0, "primitive-error-count present");
        CHECK(hash_int(cs, "error-values-stored") >= 0, "error-values-stored present");
        CHECK(hash_int(cs, "error-rate") >= 0, "error-rate present");
        CHECK(hash_int(cs, "recovery-success") >= 0, "recovery-success present");
        CHECK(hash_int(cs, "panic-recovery-count") >= 0, "panic-recovery-count present");
        CHECK(hash_int(cs, "mutation-rollback-count") >= 0, "mutation-rollback-count present");
        CHECK(hash_int(cs, "contract-violations") >= 0, "contract-violations present");
        CHECK(hash_int(cs, "error-schema") == 585, "error-schema == 585");
        CHECK(hash_int(cs, "primitives-error-total") >= 0, "primitives-error-total present");
        CHECK(hash_int(cs, "primitives-error-recommendation") >= 0,
              "primitives-error-recommendation present");
    }

    const auto errors_before = hash_int(cs, "primitive-error-count");
    const auto stored_before = hash_int(cs, "error-values-stored");
    const auto rate_before = hash_int(cs, "error-rate");

    // AC2: div0 + bad regex error injection matrix
    {
        std::println("\n--- AC2: div0 + regex error injection ---");
        auto div0 = cs.eval("(modulo 1 0)");
        CHECK(div0 && aura::compiler::types::is_error(*div0),
              "modulo div0 returns AuraError (not silent)");
        auto regex = cs.eval("(regex-match? \"[\" \"test\")");
        CHECK(regex && aura::compiler::types::is_error(*regex),
              "invalid regex returns AuraError (not silent)");
        const auto errors_after = hash_int(cs, "primitive-error-count");
        const auto stored_after = hash_int(cs, "error-values-stored");
        CHECK(errors_after > errors_before,
              std::format("primitive-error-count grew ({} -> {})", errors_before, errors_after));
        CHECK(stored_after > stored_before,
              std::format("error-values-stored grew ({} -> {})", stored_before, stored_after));
        CHECK(hash_int(cs, "error-rate") >= rate_before,
              "error-rate non-decreasing after error injection");
    }

    const auto total_before = hash_int(cs, "primitives-error-total");

    // AC3: type mismatch + mutate/query cycle
    {
        std::println("\n--- AC3: type mismatch + mutate-query cycle ---");
        auto car_err = cs.eval("(car 42)");
        CHECK(car_err && aura::compiler::types::is_error(*car_err),
              "car on non-pair returns AuraError");
        (void)cs.eval("(mutate:rebind \"acc\" \"99\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(query:pattern \"acc\")");
        const auto total_after = hash_int(cs, "primitives-error-total");
        CHECK(
            total_after >= total_before,
            std::format("primitives-error-total monotonic ({} -> {})", total_before, total_after));
        CHECK(hash_int(cs, "recovery-success") >= 0,
              "recovery-success readable after AI-agent error cycle");
    }

    // AC4: related error primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto pair_stats = cs.eval("(query:primitive-error-stats)");
        auto core = cs.eval("(query:primitives-registry-core-stats)");
        auto prim = cs.eval("(query:primitives-stats)");
        auto hotpath = cs.eval("(query:primitives-hotpath-stats)");
        CHECK(pair_stats && aura::compiler::types::is_pair(*pair_stats),
              "query:primitive-error-stats pair regression (#478)");
        CHECK(core && aura::compiler::types::is_hash(*core),
              "query:primitives-registry-core-stats hash regression (#583)");
        CHECK(prim && aura::compiler::types::is_int(*prim),
              "query:primitives-stats int regression (#583)");
        CHECK(hotpath && aura::compiler::types::is_hash(*hotpath),
              "query:primitives-hotpath-stats hash regression (#584)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 150,
              "stats:count >= 150");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}