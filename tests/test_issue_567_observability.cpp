// @category: integration
// @reason: Issue #567 — primitives-governance-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_567_detail {
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
        std::format("(hash-ref (engine:metrics \"query:primitives-governance-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_567_detail

int aura_issue_567_observability_run() {
    using namespace aura_issue_567_detail;

    std::println("=== Issue #567: primitives-governance-stats hash ===");

    aura::compiler::CompilerService cs;

    // AC1: query:primitives-governance-stats returns hash
    {
        std::println("\n--- AC1: query:primitives-governance-stats ---");
        auto stats = cs.eval("(query:primitives-governance-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:primitives-governance-stats returns hash");
        CHECK(hash_int(cs, "registry-slots") > 0, "registry-slots > 0");
        CHECK(hash_int(cs, "documented-meta-count") >= 0, "documented-meta-count present");
        CHECK(hash_int(cs, "schema-documented-count") >= 0, "schema-documented-count present");
        CHECK(hash_int(cs, "documentation-coverage-pct") >= 0,
              "documentation-coverage-pct present");
        CHECK(hash_int(cs, "capture-violations") >= 0, "capture-violations present");
        CHECK(hash_int(cs, "fastpath-hits") >= 0, "fastpath-hits present");
        CHECK(hash_int(cs, "primitive-errors") >= 0, "primitive-errors present");
        CHECK(hash_int(cs, "describe-calls") >= 0, "describe-calls present");
        CHECK(hash_int(cs, "list-meta-calls") >= 0, "list-meta-calls present");
        CHECK(hash_int(cs, "skeleton-generations") >= 0, "skeleton-generations present");
        CHECK(hash_int(cs, "eda-category-total") >= 4, "eda-category-total >= 4");
        CHECK(hash_int(cs, "extension-kit-version") == 3, "extension-kit-version == 3");
        CHECK(hash_int(cs, "governance-schema") == 567, "governance-schema == 567");
        CHECK(hash_int(cs, "primitives-governance-total") > 0, "primitives-governance-total > 0");
        CHECK(hash_int(cs, "primitives-governance-recommendation") >= 0,
              "primitives-governance-recommendation present");
    }

    const auto describe_before = hash_int(cs, "describe-calls");
    const auto fastpath_before = hash_int(cs, "fastpath-hits");
    const auto total_before = hash_int(cs, "primitives-governance-total");

    // AC2: primitive:describe bumps governance observability
    {
        std::println("\n--- AC2: primitive:describe ---");
        auto desc = cs.eval("(primitive:describe \"eda:weaken-property\")");
        CHECK(desc && aura::compiler::types::is_pair(*desc),
              "primitive:describe eda:weaken-property returns pair");
        const auto describe_after = hash_int(cs, "describe-calls");
        CHECK(describe_after > describe_before,
              std::format("describe-calls grew ({} -> {})", describe_before, describe_after));
    }

    // AC3: map/filter fastpath + skeleton generation
    {
        std::println("\n--- AC3: fastpath + skeleton ---");
        (void)cs.eval("(define big (list 1 2 3 4 5 6 7 8 9 10))");
        (void)cs.eval("(map not big)");
        const auto fastpath_after = hash_int(cs, "fastpath-hits");
        CHECK(fastpath_after > fastpath_before,
              std::format("fastpath-hits grew ({} -> {})", fastpath_before, fastpath_after));
        const auto sk_before = hash_int(cs, "skeleton-generations");
        (void)cs.eval("(primitive:generate-skeleton \"description: governance coverpoint\")");
        const auto sk_after = hash_int(cs, "skeleton-generations");
        CHECK(sk_after > sk_before,
              std::format("skeleton-generations grew ({} -> {})", sk_before, sk_after));
        const auto total_after = hash_int(cs, "primitives-governance-total");
        CHECK(total_after >= total_before,
              std::format("primitives-governance-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related primitives governance primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto reg = cs.eval("(query:primitives-registry-stats)");
        auto ext = cs.eval("(query:primitives-extension-stats)");
        auto prim = cs.eval("(query:primitives-stats)");
        auto meta = cs.eval("(query:primitive-meta-stats)");
        CHECK(reg && aura::compiler::types::is_hash(*reg),
              "query:primitives-registry-stats hash regression (#709)");
        CHECK(ext && aura::compiler::types::is_hash(*ext),
              "query:primitives-extension-stats hash regression (#697)");
        CHECK(prim && aura::compiler::types::is_int(*prim),
              "query:primitives-stats int regression (#583)");
        CHECK(meta && aura::compiler::types::is_int(*meta),
              "query:primitive-meta-stats int regression (#480)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 136,
              "stats:count >= 136");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_567_observability_run();
}
#endif
