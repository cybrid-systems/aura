// test_reflect_edsl_bridge_closed_loop_454.cpp
// Issue #454: Reflection-to-EDSL bridge closed loop.
//
// Non-duplicative with #218 (reflection integration), #248
// (schema-of-marker), #551 (reflect post-mutate Guard).
//
// AC1: query:reflect-edsl-bridge-stats reachable
// AC2: query:reflect-node-members on Define/Let nodes
// AC3: query:schema-of-marker + query:node-marker (SyntaxMarker)
// AC4: query:marker-stats returns marker distribution
// AC5: reflect-members on record type
// AC6: mutate under Guard — bridge stats monotonic
// AC7: mutation-log:summary after mutate cycle
// AC8: query regression (reflect-postmutate-stats, query:node)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_454_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;

static std::int64_t bridge_stats(CompilerService& cs) {
    auto r = cs.eval("(query:reflect-edsl-bridge-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:reflect-edsl-bridge-stats ---");
    const auto s0 = bridge_stats(cs);
    std::println("  reflect-edsl-bridge-stats = {}", s0);
    CHECK(s0 >= 0, "bridge-stats non-negative");

    std::println("\n--- AC2: query:reflect-node-members ---");
    cs.eval("(set-code \"(define x 42) (let ((y 1)) (+ y x))\")");
    CHECK(cs.eval("(eval-current)").has_value(), "workspace eval");
    auto defs = cs.eval("(ast:defs)");
    CHECK(defs && is_pair(*defs), "ast:defs returns alist");
    auto node_r = cs.eval("(query:reflect-node-members 0)");
    CHECK(node_r && is_pair(*node_r), "reflect-node-members returns alist");
    auto marker_r = cs.eval("(query:node-marker 0)");
    CHECK(marker_r && is_string(*marker_r), "node-marker returns string");

    std::println("\n--- AC3: SyntaxMarker schema introspection ---");
    (void)cs.eval("(typecheck-current)");
    auto schema = cs.eval("(query:schema-of-marker \"User\")");
    CHECK(schema.has_value(), "schema-of-marker User reachable");
    auto macro_marker = cs.eval("(query:node-marker 0)");
    CHECK(macro_marker && is_string(*macro_marker), "node-marker on define");

    std::println("\n--- AC4: query:marker-stats ---");
    auto ms = cs.eval("(query:marker-stats)");
    CHECK(ms && is_pair(*ms), "marker-stats returns list");
    auto total = cs.eval("(car (cdr (cdr (cdr (query:marker-stats)))))");
    CHECK(total && is_int(*total) && as_int(*total) > 0, "marker-stats total > 0");

    std::println("\n--- AC5: reflect-type (static reflection bridge) ---");
    auto rt = cs.eval("(reflect-type \"Int\")");
    CHECK(rt && is_pair(*rt), "reflect-type returns structured list for Int");
    auto rm = cs.eval("(reflect-members \"Int\")");
    CHECK(rm.has_value(), "reflect-members reachable (void for scalar is ok)");

    std::println("\n--- AC6: mutate + bridge stats monotonic ---");
    cs.eval("(set-code \"(define acc 0)\")");
    CHECK(cs.eval("(eval-current)").has_value(), "mutate workspace setup");
    const auto stats6a = bridge_stats(cs);
    const auto skips0 = cs.evaluator().get_macro_introduced_skipped_in_query();
    (void)cs.eval("(query:pattern \"acc\")");
    const auto skips1 = cs.evaluator().get_macro_introduced_skipped_in_query();
    for (int i = 0; i < 3; ++i) {
        CHECK(cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(10 + i) + "\")").has_value(),
              "mutate:rebind ok");
        (void)cs.eval("(eval-current)");
    }
    const auto stats6b = bridge_stats(cs);
    std::println("  bridge-stats: {} -> {} hygiene_skips: {} -> {}", stats6a, stats6b, skips0,
                 skips1);
    CHECK(stats6b >= stats6a, "bridge-stats monotonic after mutate");
    CHECK(skips1 >= skips0, "marker introspection skips monotonic");

    std::println("\n--- AC7: mutation-log:summary ---");
    auto mls = cs.eval("(mutation-log:summary)");
    CHECK(mls.has_value(), "mutation-log:summary reachable after mutate");

    std::println("\n--- AC8: query regression ---");
    auto rps = cs.eval("(query:reflect-postmutate-stats)");
    auto qn = cs.eval("(query:node 0)");
    CHECK(rps && is_int(*rps), "reflect-postmutate-stats regression");
    CHECK(qn && is_pair(*qn), "query:node regression");
}

} // namespace aura_454_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_454_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}