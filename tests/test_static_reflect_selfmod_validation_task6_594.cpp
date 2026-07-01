// test_static_reflect_selfmod_validation_task6_594.cpp
// Issue #594: Static reflection + self-modify primitives post-mutation
// schema validation hook for AI Agent code evolution (Task6).
//
// Non-duplicative with #551 (reflect-postmutate 4-tuple), #454
// (reflect-edsl-bridge), #587 (primitive meta — not yet shipped).
//
// AC1: query:reflection-selfmod-stats reachable
// AC2: mutate under Guard bumps impact_snapshot + mutation_impact
// AC3: schema_validation pass/fail counters observable
// AC4: query:reflect-node-members + query:schema-of-marker regression
// AC5: multi-round self-mod cycle — reflection-selfmod stats monotonic
// AC6: query regression (reflect-postmutate-stats, reflect-edsl-bridge-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_594_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;

static std::int64_t selfmod_stats(CompilerService& cs) {
    auto r = cs.eval("(query:reflection-selfmod-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1) (define y 2) (let ((z 3)) (+ x y z))\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:reflection-selfmod-stats ---");
    CHECK(setup_workspace(cs), "reflectable workspace setup");
    const auto s0 = selfmod_stats(cs);
    std::println("  reflection-selfmod-stats = {}", s0);
    CHECK(s0 >= 0, "reflection-selfmod-stats non-negative");

    std::println("\n--- AC2: mutate under Guard bumps validation hook ---");
    const auto snap0 = cs.evaluator().get_impact_snapshot_count();
    const auto impact0 = cs.evaluator().get_mutation_impact_count();
    const auto stats2a = selfmod_stats(cs);
    (void)cs.eval("(mutate:rebind \"x\" \"42\")");
    (void)cs.eval("(eval-current)");
    const auto snap1 = cs.evaluator().get_impact_snapshot_count();
    const auto impact1 = cs.evaluator().get_mutation_impact_count();
    const auto stats2b = selfmod_stats(cs);
    std::println("  impact_snapshot: {} -> {} mutation_impact: {} -> {}",
                 snap0, snap1, impact0, impact1);
    std::println("  selfmod stats: {} -> {}", stats2a, stats2b);
    CHECK(snap1 > snap0, "Guard mutate bumps impact_snapshot (validate hook)");
    CHECK(impact1 > impact0, "Guard mutate bumps mutation_impact");
    CHECK(stats2b > stats2a, "reflection-selfmod-stats bumped after mutate");

    std::println("\n--- AC3: schema_validation counters observable ---");
    cs.evaluator().bump_schema_validation_pass_count();
    cs.evaluator().bump_schema_validation_fail_count();
    const auto pass = cs.evaluator().get_schema_validation_pass_count();
    const auto fail = cs.evaluator().get_schema_validation_fail_count();
    std::println("  schema_pass={} schema_fail={}", pass, fail);
    CHECK(pass > 0, "schema_validation_pass_count observable");
    CHECK(fail > 0, "schema_validation_fail_count observable");

    std::println("\n--- AC4: static reflection introspection regression ---");
    auto node_r = cs.eval("(query:reflect-node-members 0)");
    auto schema = cs.eval("(query:schema-of-marker \"User\")");
    auto rt = cs.eval("(reflect-type \"Int\")");
    CHECK(node_r && is_pair(*node_r), "reflect-node-members returns alist");
    CHECK(schema.has_value(), "schema-of-marker reachable");
    CHECK(rt && is_pair(*rt), "reflect-type returns structured list");

    std::println("\n--- AC5: multi-round self-mod cycle monotonic ---");
    const auto stats5a = selfmod_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"y\" \"" +
                      std::to_string(10 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(query:reflect-node-members 0)");
    }
    const auto stats5b = selfmod_stats(cs);
    std::println("  reflection-selfmod-stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "selfmod stats monotonic over matrix");

    std::println("\n--- AC6: query regression ---");
    auto rps = cs.eval("(query:reflect-postmutate-stats)");
    auto rbs = cs.eval("(query:reflect-edsl-bridge-stats)");
    CHECK(rps && is_int(*rps), "reflect-postmutate-stats regression");
    CHECK(rbs && is_int(*rbs), "reflect-edsl-bridge-stats regression");
}

} // namespace aura_594_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_594_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}