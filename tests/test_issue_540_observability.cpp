// @category: integration
// @reason: Issue #540 — eda-stability-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_540_detail {
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
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:eda-stability-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define a 1) (define b 2) (define acc 0)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_540_detail

int aura_issue_540_observability_run() {
    using namespace aura_issue_540_detail;

    std::println("=== Issue #540: eda-stability-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup + eval-current");

    // AC1: query:eda-stability-stats returns hash
    {
        std::println("\n--- AC1: query:eda-stability-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:eda-stability-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:eda-stability-stats returns hash");
        CHECK(hash_int(cs, "cross-cow-invalidations") >= 0, "cross-cow-invalidations present");
        CHECK(hash_int(cs, "fiber-stale-ref-count") >= 0, "fiber-stale-ref-count present");
        CHECK(hash_int(cs, "provenance-mismatch") >= 0, "provenance-mismatch present");
        CHECK(hash_int(cs, "mutation-log-rollback-count") >= 0,
              "mutation-log-rollback-count present");
        CHECK(hash_int(cs, "generation-wrap-events") >= 0, "generation-wrap-events present");
        CHECK(hash_int(cs, "stable-ref-invalidations") >= 0, "stable-ref-invalidations present");
        CHECK(hash_int(cs, "node-gen-stale-accesses") >= 0, "node-gen-stale-accesses present");
        CHECK(hash_int(cs, "stale-ref-auto-refresh-count") >= 0,
              "stale-ref-auto-refresh-count present");
        CHECK(hash_int(cs, "cross-fiber-violations") >= 0, "cross-fiber-violations present");
        CHECK(hash_int(cs, "safe-resolves") >= 0, "safe-resolves present");
        CHECK(hash_int(cs, "stale-ref-blocked-count") >= 0, "stale-ref-blocked-count present");
        CHECK(hash_int(cs, "eda-stability-total") >= 0, "eda-stability-total present");
        CHECK(hash_int(cs, "eda-stability-recommendation") >= 0,
              "eda-stability-recommendation present");
    }

    const auto cross_before = hash_int(cs, "cross-cow-invalidations");
    const auto total_before = hash_int(cs, "eda-stability-total");

    // AC2: validate_stable_ref cross-COW bumps observability
    {
        std::println("\n--- AC2: validate_stable_ref cross_cow ---");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "workspace flat available");
        const auto g = ws->generation();
        (void)cs.evaluator().validate_stable_ref(0, g > 0 ? g - 1 : 0);
        const auto cross_after = hash_int(cs, "cross-cow-invalidations");
        CHECK(cross_after > cross_before,
              std::format("cross-cow-invalidations grew ({} -> {})", cross_before, cross_after));
    }

    // AC3: mutate + stable-ref query cycle
    {
        std::println("\n--- AC3: mutate + stable-ref query ---");
        CHECK(cs.eval("(mutate:rebind \"acc\" \"99\")").has_value(), "mutate:rebind under Guard");
        (void)cs.eval("(query:stable-ref 1)");
        (void)cs.eval("(query:children-stable 0)");
        const auto total_after = hash_int(cs, "eda-stability-total");
        CHECK(total_after >= total_before,
              std::format("eda-stability-total monotonic ({} -> {})", total_before, total_after));
    }

    // AC4: related stability primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto cow_fiber = cs.eval("(engine:metrics \"query:stable-ref-cow-fiber-stats\")");
        auto edsl = cs.eval("(engine:metrics \"query:edsl-stability-stats\")");
        auto lifecycle = cs.eval("(engine:metrics \"query:stable-ref-lifecycle-stats\")");
        CHECK(cow_fiber && aura::compiler::types::is_int(*cow_fiber),
              "query:stable-ref-cow-fiber-stats int regression");
        CHECK(edsl && aura::compiler::types::is_int(*edsl),
              "query:edsl-stability-stats int regression");
        CHECK(lifecycle && aura::compiler::types::is_hash(*lifecycle),
              "query:stable-ref-lifecycle-stats hash regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 133,
              "stats:count >= 133");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_540_observability_run();
}
#endif
