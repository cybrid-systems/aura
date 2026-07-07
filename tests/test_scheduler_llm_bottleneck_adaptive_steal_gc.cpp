// test_scheduler_llm_bottleneck_adaptive_steal_gc.cpp — Issue #754:
// LLM-bottleneck adaptive scheduling + yield-classification metrics +
// work-stealing bias + GC safepoint self-tuning observability
// (refines #730; non-duplicative with #706 adaptive-stats, #650
// yield-class-stats, #646 gc-safepoint-deferral-stats).
//
//   - AC1:  query:orchestration-llm-bottleneck-stats reachable (schema 754)
//   - AC2:  gc-safepoint-adapted bumps on direct path
//   - AC3:  gc-safepoint-adapted bumps on request_gc_safepoint defer
//   - AC4:  orchestration-events-total == sum of 4 per-counter fields
//   - AC5:  MutationBoundaryGuard + request_gc_safepoint real exercise
//   - AC6:  mutate:request-gc-safepoint no-guard path does not bump adapted
//   - AC7:  orchestration-events-total monotonic over exercise matrix
//   - AC8:  query regression (#706 adaptive-stats, #646 gc-deferral)

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_754_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t stat_int(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (query:orchestration-llm-bottleneck-stats) '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t outermost_preferred(CompilerService& cs) {
    return stat_int(cs, "outermost-preferred");
}
static std::int64_t backoff_triggers(CompilerService& cs) {
    return stat_int(cs, "backoff-triggers");
}
static std::int64_t llm_tail_reduction(CompilerService& cs) {
    return stat_int(cs, "llm-tail-reduction");
}
static std::int64_t gc_safepoint_adapted(CompilerService& cs) {
    return stat_int(cs, "gc-safepoint-adapted");
}
static std::int64_t events_total(CompilerService& cs) {
    return stat_int(cs, "orchestration-events-total");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:orchestration-llm-bottleneck-stats (schema 754) ---");
    auto h = cs.eval("(query:orchestration-llm-bottleneck-stats)");
    CHECK(h && is_hash(*h), "orchestration-llm-bottleneck-stats returns hash");
    CHECK(stat_int(cs, "schema") == 754, "schema == 754");
    CHECK(outermost_preferred(cs) >= 0, "outermost-preferred non-negative");
    CHECK(backoff_triggers(cs) >= 0, "backoff-triggers non-negative");
    CHECK(llm_tail_reduction(cs) >= 0, "llm-tail-reduction non-negative");
    CHECK(gc_safepoint_adapted(cs) >= 0, "gc-safepoint-adapted non-negative");

    std::println("\n--- AC2: gc-safepoint-adapted bumps on direct path ---");
    const auto g0 = gc_safepoint_adapted(cs);
    cs.evaluator().bump_orchestration_llm_gc_safepoint_adapted();
    cs.evaluator().bump_orchestration_llm_gc_safepoint_adapted();
    const auto g1 = gc_safepoint_adapted(cs);
    CHECK(g1 == g0 + 2, "gc-safepoint-adapted bumps by exactly 2");

    std::println("\n--- AC3: request_gc_safepoint defer bumps gc-safepoint-adapted ---");
    const auto g2a = gc_safepoint_adapted(cs);
    bool guard_ok = false;
    {
        Evaluator::MutationBoundaryGuard guard(cs.evaluator(), &guard_ok);
        const int code = cs.evaluator().request_gc_safepoint();
        CHECK(code == 1, "request_gc_safepoint returns 1 (deferred) under guard");
    }
    const auto g2b = gc_safepoint_adapted(cs);
    CHECK(g2b == g2a + 1, "gc-safepoint-adapted bumps by 1 on defer path");

    std::println("\n--- AC4: orchestration-events-total == sum ---");
    const auto o = outermost_preferred(cs);
    const auto b = backoff_triggers(cs);
    const auto l = llm_tail_reduction(cs);
    const auto g = gc_safepoint_adapted(cs);
    const auto tot = events_total(cs);
    CHECK(tot == o + b + l + g, "orchestration-events-total == sum of 4 counters");

    std::println("\n--- AC5: MutationBoundaryGuard + nested defer exercise ---");
    const auto ev5a = events_total(cs);
    const auto g5a = gc_safepoint_adapted(cs);
    bool outer_ok = false;
    bool inner_ok = false;
    {
        Evaluator::MutationBoundaryGuard outer(cs.evaluator(), &outer_ok);
        (void)cs.evaluator().request_gc_safepoint();
        {
            Evaluator::MutationBoundaryGuard inner(cs.evaluator(), &inner_ok);
            (void)cs.evaluator().request_gc_safepoint();
        }
        (void)cs.evaluator().request_gc_safepoint();
    }
    CHECK(gc_safepoint_adapted(cs) == g5a + 3,
          "gc-safepoint-adapted grew by 3 over nested guard exercise");
    CHECK(events_total(cs) == ev5a + 3, "orchestration-events-total grew by 3");

    std::println("\n--- AC6: mutate:request-gc-safepoint no-guard path ---");
    const auto g6a = gc_safepoint_adapted(cs);
    auto immediate = cs.eval("(mutate:request-gc-safepoint)");
    CHECK(immediate && is_int(*immediate), "mutate:request-gc-safepoint returns int");
    if (immediate && is_int(*immediate)) {
        const auto code = as_int(*immediate);
        CHECK(code == 0, "mutate:request-gc-safepoint returns 0 without guard");
        CHECK(gc_safepoint_adapted(cs) == g6a,
              "gc-safepoint-adapted unchanged on immediate safepoint path");
    }

    std::println("\n--- AC7: orchestration-events-total monotonic ---");
    const auto ev7a = events_total(cs);
    cs.evaluator().bump_orchestration_llm_gc_safepoint_adapted();
    CHECK(events_total(cs) > ev7a, "orchestration-events-total monotonic after bump");

    std::println("\n--- AC8: query regression ---");
    auto adaptive = cs.eval("(query:scheduler-stealbudget-adaptive-stats)");
    auto gc_defer = cs.eval("(query:gc-safepoint-deferral-stats)");
    CHECK(adaptive && is_hash(*adaptive), "scheduler-stealbudget-adaptive-stats regression (#706)");
    CHECK(gc_defer && is_hash(*gc_defer), "gc-safepoint-deferral-stats regression (#646)");
}

} // namespace aura_issue_754_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_issue_754_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}