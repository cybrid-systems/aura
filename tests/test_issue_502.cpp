// @category: integration
// @reason: Issue #502 — Post-mutate reflect validation + Guard impact snapshot

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_502_detail {
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

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define a 1) (define b 2) (+ a b)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_502_detail

int aura_issue_502_run() {
    using namespace aura_issue_502_detail;

    std::println("=== Issue #502: Post-mutate reflect validation + snapshot ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup");

    const auto pass_before = cs.evaluator().get_schema_validation_pass_count();
    const auto snap_before = cs.evaluator().get_impact_snapshot_count();

    // AC1: query:reflect-postmutate-stats returns hash
    {
        std::println("\n--- AC1: query:reflect-postmutate-stats ---");
        auto stats = cs.eval("(query:reflect-postmutate-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:reflect-postmutate-stats returns hash");
    }

    // AC2: Guard mutate wires post_mutation_reflect_validate + snapshot bump
    {
        std::println("\n--- AC2: Guard dtor validate hook ---");
        CHECK(cs.eval("(mutate:rebind \"a\" \"10\")").has_value(), "mutate:rebind under Guard");
        const auto pass_after = cs.evaluator().get_schema_validation_pass_count();
        const auto snap_after = cs.evaluator().get_impact_snapshot_count();
        CHECK(pass_after > pass_before,
              std::format("schema_validation_pass grew ({} -> {})", pass_before, pass_after));
        CHECK(snap_after > snap_before,
              std::format("impact_snapshot_count grew ({} -> {})", snap_before, snap_after));
        CHECK(cs.evaluator().get_last_schema_validation_ok(),
              "last_schema_validation_ok after healthy mutate");
    }

    // AC3: query:mutation-impact-snapshot consumable (top-level hash call)
    {
        std::println("\n--- AC3: mutation-impact-snapshot ---");
        auto snap = cs.eval("(query:mutation-impact-snapshot)");
        CHECK(snap && aura::compiler::types::is_hash(*snap),
              "query:mutation-impact-snapshot returns hash");
        CHECK(cs.evaluator().get_mutation_impact_count() >= 1,
              "mutation_impact_count >= 1 after mutate");
    }

    // AC4: mutate-then-query self-evolution cycle
    {
        std::println("\n--- AC4: mutate-then-query cycle ---");
        const auto impact_before = cs.evaluator().get_mutation_impact_count();
        CHECK(cs.eval("(mutate:rebind \"b\" \"20\")").has_value(), "second mutate under Guard");
        (void)cs.eval("(query:pattern \"a\")");
        auto stats = cs.eval("(query:reflect-postmutate-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "reflect-postmutate-stats hash after cycle");
        CHECK(cs.evaluator().get_mutation_impact_count() > impact_before,
              "mutation_impact_count grew over cycle");
    }

    // AC5: non-mutate path regression (no false schema-fail)
    {
        std::println("\n--- AC5: query-only regression ---");
        aura::compiler::CompilerService cs2;
        CHECK(setup_workspace(cs2), "fresh workspace for query-only path");
        const auto fail0 = cs2.evaluator().get_schema_validation_fail_count();
        (void)cs2.eval("(query:pattern \"a\")");
        auto stats = cs2.eval("(query:reflect-postmutate-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "reflect-postmutate-stats hash on query-only path");
        CHECK(cs2.evaluator().get_schema_validation_fail_count() == fail0,
              "schema_validation_fail unchanged on query-only path");
    }

    // AC6: related primitive regression
    {
        std::println("\n--- AC6: related stats regression ---");
        auto mi = cs.eval("(query:mutation-impact)");
        auto rself = cs.eval("(query:reflection-selfmod-stats)");
        CHECK(mi && aura::compiler::types::is_int(*mi), "mutation-impact regression");
        CHECK(rself && aura::compiler::types::is_int(*rself),
              "reflection-selfmod-stats regression");
    }

    // AC7: stats:count
    {
        std::println("\n--- AC7: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 99,
              "stats:count >= 99");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_502_run();
}
#endif
