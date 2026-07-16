// @category: integration
// @reason: Issue #525 — guard-production-impact-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_525_detail {
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
        "(hash-ref (engine:metrics \"query:guard-production-impact-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define a 1) (define b 2) (+ a b)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_525_detail

int aura_issue_525_observability_run() {
    using namespace aura_issue_525_detail;

    std::println("=== Issue #525: guard-production-impact-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup");

    // AC1: query:guard-production-impact-stats returns hash
    {
        std::println("\n--- AC1: query:guard-production-impact-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:guard-production-impact-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:guard-production-impact-stats returns hash");
        CHECK(hash_int(cs, "epoch-after") >= 0, "epoch-after present");
        CHECK(hash_int(cs, "epoch-delta") >= 0, "epoch-delta present");
        CHECK(hash_int(cs, "nodes-changed") >= 0, "nodes-changed present");
        CHECK(hash_int(cs, "reasons-mask") >= 0, "reasons-mask present");
        CHECK(hash_int(cs, "impact-snapshots") >= 0, "impact-snapshots present");
        CHECK(hash_int(cs, "mutation-impacts") >= 0, "mutation-impacts present");
        CHECK(hash_int(cs, "dirty-nodes") >= 0, "dirty-nodes present");
        CHECK(hash_int(cs, "macro-markers") >= 0, "macro-markers present");
        CHECK(hash_int(cs, "schema-pass") >= 0, "schema-pass present");
        CHECK(hash_int(cs, "schema-fail") >= 0, "schema-fail present");
        CHECK(hash_int(cs, "schema-valid") >= 0, "schema-valid present");
        CHECK(hash_int(cs, "guard-epoch") >= 0, "guard-epoch present");
        CHECK(hash_int(cs, "boundary-depth") >= 0, "boundary-depth present");
        CHECK(hash_int(cs, "dirty-propagation") >= 0, "dirty-propagation present");
        CHECK(hash_int(cs, "checkpoint-commits") >= 0, "checkpoint-commits present");
        CHECK(hash_int(cs, "validation-pass-rate-pct") >= 0, "validation-pass-rate-pct present");
        CHECK(hash_int(cs, "guard-production-impact-total") >= 0,
              "guard-production-impact-total present");
        CHECK(hash_int(cs, "guard-production-impact-recommendation") >= 0,
              "guard-production-impact-recommendation present");
    }

    const auto total_before = hash_int(cs, "guard-production-impact-total");
    const auto snap_before = cs.evaluator().get_impact_snapshot_count();
    const auto pass_before = cs.evaluator().get_schema_validation_pass_count();

    // AC2: Guard mutate bumps snapshot + schema validation counters
    {
        std::println("\n--- AC2: Guard success impact + validation ---");
        CHECK(cs.eval("(mutate:rebind \"a\" \"10\")").has_value(), "mutate:rebind under Guard");
        const auto snap_after = cs.evaluator().get_impact_snapshot_count();
        const auto pass_after = cs.evaluator().get_schema_validation_pass_count();
        CHECK(snap_after > snap_before,
              std::format("impact_snapshot grew ({} -> {})", snap_before, snap_after));
        CHECK(pass_after > pass_before,
              std::format("schema_validation_pass grew ({} -> {})", pass_before, pass_after));
        CHECK(hash_int(cs, "schema-valid") == 1, "schema-valid after healthy mutate");
        CHECK(hash_int(cs, "impact-snapshots") >= static_cast<std::int64_t>(snap_after),
              "hash impact-snapshots reflects bump");
    }

    // AC3: mutate batch → production impact dashboard
    {
        std::println("\n--- AC3: mutate batch ---");
        CHECK(cs.eval("(mutate:rebind \"b\" \"20\")").has_value(), "second mutate under Guard");
        const auto total_after = hash_int(cs, "guard-production-impact-total");
        CHECK(total_after >= total_before,
              std::format("guard-production-impact-total monotonic ({} -> {})", total_before,
                          total_after));
        CHECK(hash_int(cs, "mutation-impacts") >= 2, "mutation-impacts reflects batch");
    }

    // AC4: related Guard/reflect primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto log = cs.eval("(engine:metrics \"query:mutation-boundary-log\")");
        auto snap = cs.eval("(engine:metrics \"query:mutation-impact-snapshot\")");
        auto reflect = cs.eval("(engine:metrics \"query:reflect-postmutate-stats\")");
        CHECK(log && aura::compiler::types::is_hash(*log), "mutation-boundary-log hash regression");
        CHECK(snap && aura::compiler::types::is_hash(*snap),
              "mutation-impact-snapshot hash regression");
        CHECK(reflect && aura::compiler::types::is_hash(*reflect),
              "reflect-postmutate-stats hash regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 122,
              "stats:count >= 122");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_525_observability_run();
}
#endif
