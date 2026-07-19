// @category: integration
// @reason: Issue #583 — primitives-registry-core-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_583_detail {
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
        "(hash-ref (engine:metrics \"query:primitives-registry-core-stats\") '{}')", key));
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

} // namespace aura_issue_583_detail

int main() {
    using namespace aura_issue_583_detail;

    std::println("=== Issue #583: primitives-registry-core-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "list workspace setup");

    // AC1: query:primitives-registry-core-stats returns hash
    {
        std::println("\n--- AC1: query:primitives-registry-core-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:primitives-registry-core-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:primitives-registry-core-stats returns hash");
        CHECK(hash_int(cs, "registry-slots") > 0, "registry-slots > 0");
        CHECK(hash_int(cs, "primitive-call-count") >= 0, "primitive-call-count present");
        CHECK(hash_int(cs, "error-rate-pct") >= 0, "error-rate-pct present");
        CHECK(hash_int(cs, "hot-path-hits") >= 0, "hot-path-hits present");
        CHECK(hash_int(cs, "error-path-cost") >= 0, "error-path-cost present");
        CHECK(hash_int(cs, "consistency-violations") >= 0, "consistency-violations present");
        CHECK(hash_int(cs, "registry-core-schema") == 583, "registry-core-schema == 583");
        CHECK(hash_int(cs, "primitives-registry-core-total") > 0,
              "primitives-registry-core-total > 0");
        CHECK(hash_int(cs, "primitives-registry-core-recommendation") >= 0,
              "primitives-registry-core-recommendation present");
    }

    const auto calls_before = hash_int(cs, "primitive-call-count");
    const auto errors_before = hash_int(cs, "error-path-cost");
    const auto total_before = hash_int(cs, "primitives-registry-core-total");

    // AC2: core list + math error path
    {
        std::println("\n--- AC2: list + math error path ---");
        auto car_r = cs.eval("(car xs)");
        CHECK(car_r && aura::compiler::types::is_int(*car_r) &&
                  aura::compiler::types::as_int(*car_r) == 1,
              "car returns head element");
        (void)cs.eval("(modulo 1 0)");
        const auto errors_after = hash_int(cs, "error-path-cost");
        CHECK(errors_after > errors_before,
              std::format("error-path-cost grew ({} -> {})", errors_before, errors_after));
    }

    // AC3: mutate + query hot-path cycle
    {
        std::println("\n--- AC3: mutate + query cycle ---");
        (void)cs.eval("(mutate:rebind \"acc\" \"42\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(query:pattern \"acc\")");
        const auto calls_after = hash_int(cs, "primitive-call-count");
        const auto total_after = hash_int(cs, "primitives-registry-core-total");
        CHECK(calls_after > calls_before,
              std::format("primitive-call-count grew ({} -> {})", calls_before, calls_after));
        CHECK(total_after >= total_before,
              std::format("primitives-registry-core-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related primitives primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto prim = cs.eval("(engine:metrics \"query:primitives-stats\")");
        auto gov = cs.eval("(engine:metrics \"query:primitives-governance-stats\")");
        auto reg = cs.eval("(engine:metrics \"query:primitives-registry-stats\")");
        auto err = cs.eval("(engine:metrics \"query:primitive-error-stats\")");
        CHECK(prim && aura::compiler::types::is_int(*prim),
              "query:primitives-stats int regression (#583)");
        CHECK(gov && aura::compiler::types::is_hash(*gov),
              "query:primitives-governance-stats hash regression (#567)");
        CHECK(reg && aura::compiler::types::is_hash(*reg),
              "query:primitives-registry-stats hash regression (#709)");
        CHECK(err.has_value(), "query:primitive-error-stats regression (#478)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 148,
              "stats:count >= 148");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}