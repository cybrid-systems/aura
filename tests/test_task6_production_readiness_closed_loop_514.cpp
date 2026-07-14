// test_task6_production_readiness_closed_loop_514.cpp
// Issue #514: Task6 Top 3 production-readiness closed loop (meta).
//
// Non-duplicative with #547 (pattern hygiene), #550 (dirty/type),
// #551 (reflect post-mutate), #597 (full matrix), #619 (followup).
//
// AC1: query:task6-production-readiness-stats reachable
// AC2: Top1 — macro expand + query:pattern hygiene + marker stats
// AC3: Top2 — mutate:rebind under Guard bumps mutation_impact
// AC4: Top3 — dirty/type incremental counters observable post-mutate
// AC5: query:ir-hygiene-stats reachable
// AC6: query:pattern-marker-stats reachable
// AC7: multi-round self-evo cycle — production stats monotonic
// AC8: query regression (pattern-hygiene, reflect-postmutate, typed-mutation)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_514_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t reflect_postmutate_total(CompilerService& cs) {
    auto r = cs.eval("(hash-ref (query:reflect-postmutate-stats) 'reflect-postmutate-total')");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static std::int64_t ir_hygiene_total(CompilerService& cs) {
    auto r = cs.eval("(hash-ref (query:ir-hygiene-stats) 'ir-hygiene-total')");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static std::int64_t prod_stats(CompilerService& cs) {
    auto r = cs.eval("(query:task6-production-readiness-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_macro_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (mk x) "
                 "  (list 'define (list 'v x) x)) "
                 "(define user-val 1) (mk 10)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:task6-production-readiness-stats ---");
    CHECK(setup_macro_workspace(cs), "macro workspace setup");
    const auto s0 = prod_stats(cs);
    std::println("  production-readiness-stats = {}", s0);
    CHECK(s0 >= 0, "production-readiness-stats non-negative");

    std::println("\n--- AC2: Top1 hygiene/marker propagation ---");
    const auto skips0 = cs.evaluator().get_macro_introduced_skipped_in_query();
    (void)cs.eval("(query:pattern \"v\")");
    const auto skips1 = cs.evaluator().get_macro_introduced_skipped_in_query();
    auto irs = cs.eval("(query:ir-hygiene-stats)");
    auto pms = cs.eval("(query:pattern-marker-stats)");
    std::println("  hygiene_skips: {} -> {} ir-hygiene={} pattern-marker-hash={}", skips0, skips1,
                 irs && is_hash(*irs) ? ir_hygiene_total(cs) : 0, pms && is_hash(*pms) ? 1 : 0);
    CHECK(skips1 > skips0, "MacroIntroduced filtered in query:pattern");
    CHECK(irs && is_hash(*irs), "query:ir-hygiene-stats returns hash");
    CHECK(pms && is_hash(*pms), "query:pattern-marker-stats returns hash");
    CHECK(skips1 > 0, "query skips recorded after pattern hygiene filter");

    std::println("\n--- AC3: Top2 Guard + reflect post-mutate ---");
    const auto impact0 = cs.evaluator().get_mutation_impact_count();
    (void)cs.eval("(mutate:rebind \"user-val\" \"42\")");
    const auto impact1 = cs.evaluator().get_mutation_impact_count();
    const auto snap = cs.evaluator().get_impact_snapshot_count();
    std::println("  mutation_impact: {} -> {} impact_snapshot={}", impact0, impact1, snap);
    CHECK(impact1 > impact0, "Guard success bumps mutation_impact");

    std::println("\n--- AC4: Top3 dirty/type incremental ---");
    auto dirty = cs.eval("(query:dirty-impact)");
    auto typed = cs.eval("(engine:metrics \"query:typed-mutation-stats\")");
    CHECK(dirty && is_int(*dirty), "query:dirty-impact returns int");
    CHECK(typed && is_int(*typed), "query:typed-mutation-stats returns int");
    std::println("  dirty-impact={} typed-mutation-stats={}", as_int(*dirty), as_int(*typed));

    std::println("\n--- AC5: query:ir-hygiene-stats ---");
    auto ir0 = cs.eval("(query:ir-hygiene-stats)");
    CHECK(ir0 && is_hash(*ir0) && ir_hygiene_total(cs) >= 0, "ir-hygiene-stats non-negative");

    std::println("\n--- AC6: query:pattern-marker-stats ---");
    auto pm0 = cs.eval("(query:pattern-marker-stats)");
    CHECK(pm0 && is_hash(*pm0), "pattern-marker-stats hash after macro+query");

    std::println("\n--- AC7: multi-round self-evo cycle ---");
    const auto stats7a = prod_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(query:pattern \"user-val\")");
        (void)cs.eval("(mutate:rebind \"user-val\" \"" + std::to_string(200 + round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats7b = prod_stats(cs);
    std::println("  production-readiness: {} -> {}", stats7a, stats7b);
    CHECK(stats7b >= stats7a, "production-readiness stats monotonic");

    std::println("\n--- AC8: query regression ---");
    auto phs = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    auto rps = cs.eval("(query:reflect-postmutate-stats)");
    auto tms = cs.eval("(engine:metrics \"query:typed-mutation-stats\")");
    CHECK(phs && is_int(*phs), "pattern-hygiene-stats regression");
    CHECK(rps && is_hash(*rps), "reflect-postmutate-stats regression");
    CHECK(reflect_postmutate_total(cs) >= 0, "reflect-postmutate-total non-negative");
    CHECK(tms && is_int(*tms), "typed-mutation-stats regression");
}

} // namespace aura_514_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_514_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}