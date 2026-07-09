// @category: integration
// @reason: Issue #504 — MutationBoundaryGuard impact log + query primitive

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_504_detail {
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

} // namespace aura_issue_504_detail

int aura_issue_504_run() {
    using namespace aura_issue_504_detail;

    std::println("=== Issue #504: MutationBoundaryGuard impact log ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup");

    const auto impact_before = cs.evaluator().get_mutation_impact_count();
    const auto snap_before = cs.evaluator().get_impact_snapshot_count();
    const auto ring_before = cs.evaluator().get_mutation_impact_ring_seq();

    // AC1: query:mutation-boundary-log returns hash
    {
        std::println("\n--- AC1: query:mutation-boundary-log ---");
        auto log = cs.eval("(query:mutation-boundary-log)");
        CHECK(log && aura::compiler::types::is_hash(*log),
              "query:mutation-boundary-log returns hash");
    }

    // AC2: Guard mutate produces usable impact snapshot
    {
        std::println("\n--- AC2: Guard success impact snapshot ---");
        CHECK(cs.eval("(mutate:rebind \"a\" \"10\")").has_value(), "mutate:rebind under Guard");
        const auto impact_after = cs.evaluator().get_mutation_impact_count();
        const auto snap_after = cs.evaluator().get_impact_snapshot_count();
        const auto ring_after = cs.evaluator().get_mutation_impact_ring_seq();
        CHECK(impact_after > impact_before,
              std::format("mutation_impact grew ({} -> {})", impact_before, impact_after));
        CHECK(snap_after > snap_before,
              std::format("impact_snapshot grew ({} -> {})", snap_before, snap_after));
        CHECK(ring_after > ring_before,
              std::format("ring_seq grew ({} -> {})", ring_before, ring_after));
        auto log = cs.eval("(query:mutation-boundary-log)");
        CHECK(log && aura::compiler::types::is_hash(*log), "boundary-log hash after Guard mutate");
    }

    // AC3: AI loop can consume log without extra snapshot queries
    {
        std::println("\n--- AC3: mutate batch → boundary log ---");
        const auto impact_batch = cs.evaluator().get_mutation_impact_count();
        CHECK(cs.eval("(mutate:rebind \"b\" \"20\")").has_value(), "second mutate under Guard");
        auto log = cs.eval("(query:mutation-boundary-log)");
        CHECK(log && aura::compiler::types::is_hash(*log),
              "boundary-log consumable after batch mutate");
        CHECK(cs.evaluator().get_mutation_impact_count() > impact_batch,
              "mutation_impact grew in batch");
    }

    // AC4: mutate-then-query self-evolution cycle
    {
        std::println("\n--- AC4: mutate-then-query cycle ---");
        CHECK(cs.eval("(mutate:rebind \"a\" \"30\")").has_value(), "third mutate under Guard");
        (void)cs.eval("(query:pattern \"a\")");
        auto log = cs.eval("(query:mutation-boundary-log)");
        CHECK(log && aura::compiler::types::is_hash(*log), "boundary-log hash after query cycle");
    }

    // AC5: query-only path zero regression on Guard safety
    {
        std::println("\n--- AC5: query-only regression ---");
        aura::compiler::CompilerService cs2;
        CHECK(setup_workspace(cs2), "fresh workspace for query-only path");
        const auto impact0 = cs2.evaluator().get_mutation_impact_count();
        (void)cs2.eval("(query:pattern \"a\")");
        auto log = cs2.eval("(query:mutation-boundary-log)");
        CHECK(log && aura::compiler::types::is_hash(*log), "boundary-log hash on query-only path");
        CHECK(cs2.evaluator().get_mutation_impact_count() == impact0,
              "mutation_impact unchanged on query-only path");
    }

    // AC6: related primitive regression
    {
        std::println("\n--- AC6: related stats regression ---");
        // Re-register this service's Evaluator as the active
        // query-evaluator for the current thread. The previous
        // inner-scope `cs2` (AC5) cleared g_query_evaluator in
        // its destructor (Issue #226: unbind thread-local on
        // ~Evaluator), so the thread-local slot is null by
        // here. Without re-registration, query:mutation-impact
        // / query:mutation-impact-snapshot would see nullptr.
        aura::compiler::Evaluator::set_query_evaluator(&cs.evaluator());
        auto mis = cs.eval("(query:mutation-impact-snapshot)");
        auto mi = cs.eval("(query:mutation-impact)");
        CHECK(mis && aura::compiler::types::is_hash(*mis), "mutation-impact-snapshot regression");
        CHECK(mi && aura::compiler::types::is_int(*mi), "mutation-impact regression");
    }

    // AC7: stats:count
    {
        std::println("\n--- AC7: stats:count ---");
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
    return aura_issue_504_run();
}
#endif
