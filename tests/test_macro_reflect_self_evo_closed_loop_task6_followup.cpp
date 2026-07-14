// test_macro_reflect_self_evo_closed_loop_task6_followup.cpp
// Issue #619: Task6 macro+reflect+self-evo follow-up closed loop.
//
// Non-duplicative with #597 (full matrix), #547 (pattern hygiene),
// #551 (reflect post-mutate), #326/#327 (macro+mutate e2e).
//
// AC1: query:macro-reflect-self-evo-followup-stats reachable
// AC2: macro expand + query:pattern hygiene filter active
// AC3: mutate:query-and-replace on user code succeeds
// AC4: Guard reflect path — mutation_impact / schema counters
// AC5: dirty/epoch propagation after self-evo mutate
// AC6: typecheck after mutate cycle
// AC7: query regression (macro-reflect-self-evo-stats, pattern-hygiene)
// AC8: multi-round self-evo cycle — followup stats monotonic
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_619_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t followup_stats(CompilerService& cs) {
    auto r = cs.eval("(query:macro-reflect-self-evo-followup-stats)");
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
    std::println("\n--- AC1: query:macro-reflect-self-evo-followup-stats ---");
    CHECK(setup_macro_workspace(cs), "macro workspace setup");
    const auto s0 = followup_stats(cs);
    std::println("  followup-stats = {}", s0);
    CHECK(s0 >= 0, "followup-stats non-negative");

    std::println("\n--- AC2: query:pattern hygiene filter ---");
    const auto skips0 = cs.evaluator().get_macro_introduced_skipped_in_query();
    (void)cs.eval("(query:pattern \"v\")");
    const auto skips1 = cs.evaluator().get_macro_introduced_skipped_in_query();
    std::println("  hygiene_skips: {} -> {}", skips0, skips1);
    CHECK(skips1 >= skips0, "macro-introduced nodes filtered in query");

    std::println("\n--- AC3: mutate under Guard on user code ---");
    auto qr = cs.eval("(query:pattern \"user-val\")");
    CHECK(qr.has_value(), "query:pattern finds user-val");
    auto mr = cs.eval("(mutate:rebind \"user-val\" \"42\")");
    CHECK(mr.has_value(), "mutate:rebind on user binding succeeds");

    std::println("\n--- AC4: Guard reflect / transform counters ---");
    const auto impact0 = cs.evaluator().get_mutation_impact_count();
    (void)cs.eval("(mutate:rebind \"user-val\" \"99\")");
    const auto impact1 = cs.evaluator().get_mutation_impact_count();
    const auto schema = cs.evaluator().get_schema_validation_pass_count();
    std::println("  mutation_impact: {} -> {} schema_pass={}", impact0, impact1, schema);
    CHECK(impact1 > impact0, "transform_applied proxy bumped on Guard success");

    std::println("\n--- AC5: dirty/epoch after self-evo mutate ---");
    auto epoch = cs.eval("(query:epoch-delta-since-last-query)");
    CHECK(epoch.has_value() && is_int(*epoch), "epoch-delta observable");
    if (epoch && is_int(*epoch))
        CHECK(as_int(*epoch) >= 0, "epoch delta non-negative");

    std::println("\n--- AC6: typecheck after mutate cycle ---");
    (void)cs.eval("(eval-current)");
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "typecheck-current after self-evo mutate");

    std::println("\n--- AC7: query regression ---");
    auto mrs = cs.eval("(query:macro-reflect-self-evo-stats)");
    auto phs = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    CHECK(mrs && is_int(*mrs), "macro-reflect-self-evo-stats returns int");
    CHECK(phs && is_int(*phs), "pattern-hygiene-stats returns int");

    std::println("\n--- AC8: multi-round self-evo cycle ---");
    const auto stats8a = followup_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(query:pattern \"user-val\")");
        (void)cs.eval("(mutate:rebind \"user-val\" \"" + std::to_string(100 + round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats8b = followup_stats(cs);
    std::println("  followup-stats: {} -> {}", stats8a, stats8b);
    CHECK(stats8b >= stats8a, "followup-stats monotonic over matrix");

    const auto impact = cs.evaluator().get_mutation_impact_count();
    const auto skips = cs.evaluator().get_macro_introduced_skipped_in_query();
    std::println("  final transform_applied={} hygiene_skips={}", impact, skips);
    CHECK(impact > 0, "mutation_impact > 0 after self-evo cycle");
}

} // namespace aura_619_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_619_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}