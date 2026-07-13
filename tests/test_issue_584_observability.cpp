// @category: integration
// @reason: Issue #584 — primitives-hotpath-stats AI-agent stress slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_584_detail {
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
        std::format("(hash-ref (engine:metrics \"query:primitives-hotpath-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_584_detail

int main() {
    using namespace aura_issue_584_detail;

    std::println("=== Issue #584: primitives-hotpath-stats AI-agent stress ===");

    aura::compiler::CompilerService cs;

    // AC1: query:primitives-hotpath-stats returns hash with #584 fields
    {
        std::println("\n--- AC1: query:primitives-hotpath-stats ---");
        auto stats = cs.eval("(query:primitives-hotpath-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:primitives-hotpath-stats returns hash");
        CHECK(hash_int(cs, "primitive-call-total") >= 0, "primitive-call-total present (#614)");
        CHECK(hash_int(cs, "pair-alloc-total") >= 0, "pair-alloc-total present (#614)");
        CHECK(hash_int(cs, "call-rate") >= 0, "call-rate present");
        CHECK(hash_int(cs, "alloc-per-call") >= 0, "alloc-per-call present");
        CHECK(hash_int(cs, "regex-time-us") >= 0, "regex-time-us present");
        CHECK(hash_int(cs, "stability-score") >= 0, "stability-score present");
        CHECK(hash_int(cs, "hotpath-schema") == 584, "hotpath-schema == 584");
        CHECK(hash_int(cs, "primitives-hotpath-total") >= 0, "primitives-hotpath-total present");
        CHECK(hash_int(cs, "primitives-hotpath-recommendation") >= 0,
              "primitives-hotpath-recommendation present");
    }

    const auto pair_before = hash_int(cs, "pair-alloc-total");
    const auto call_before = hash_int(cs, "primitive-call-total");
    const auto total_before = hash_int(cs, "primitives-hotpath-total");

    // AC2: dynamic list construction bumps hot-path counters
    {
        std::println("\n--- AC2: dynamic list + map workload ---");
        cs.eval("(define build-list (lambda (n) (let loop ((i 0) (acc '())) "
                "(if (= i n) acc (loop (+ i 1) (append acc (list (+ i 1))))))))");
        auto len = cs.eval("(length (build-list 10))");
        CHECK(len && aura::compiler::types::is_int(*len) &&
                  aura::compiler::types::as_int(*len) == 10,
              "build-list 10 has length 10");
        cs.eval("(define big-list (build-list 15))");
        (void)cs.eval("(map (lambda (x) (* x 2)) big-list)");
        const auto pair_after = hash_int(cs, "pair-alloc-total");
        const auto call_after = hash_int(cs, "primitive-call-total");
        CHECK(pair_after > pair_before,
              std::format("pair-alloc-total grew ({} -> {})", pair_before, pair_after));
        CHECK(
            call_after >= call_before,
            std::format("primitive-call-total non-decreasing ({} -> {})", call_before, call_after));
    }

    // AC3: regex + mutate-query-eval cycle
    {
        std::println("\n--- AC3: regex + mutate-query cycle ---");
        (void)cs.eval("(regex-split \"a,b,c\" \",\")");
        (void)cs.eval("(set-code \"(define acc 0)\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(mutate:rebind \"acc\" \"7\")");
        (void)cs.eval("(query:pattern \"acc\")");
        const auto total_after = hash_int(cs, "primitives-hotpath-total");
        CHECK(total_after >= total_before,
              std::format("primitives-hotpath-total monotonic ({} -> {})", total_before,
                          total_after));
        CHECK(hash_int(cs, "stability-score") >= 0,
              "stability-score readable after AI-agent cycle");
    }

    // AC4: related primitives hot-path regression
    {
        std::println("\n--- AC4: regression ---");
        auto core = cs.eval("(query:primitives-registry-core-stats)");
        auto gov = cs.eval("(query:primitives-governance-stats)");
        auto prim = cs.eval("(query:primitives-stats)");
        CHECK(core && aura::compiler::types::is_hash(*core),
              "query:primitives-registry-core-stats hash regression (#583)");
        CHECK(gov && aura::compiler::types::is_hash(*gov),
              "query:primitives-governance-stats hash regression (#567)");
        CHECK(prim && aura::compiler::types::is_int(*prim),
              "query:primitives-stats int regression (#583)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 149,
              "stats:count >= 149");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}