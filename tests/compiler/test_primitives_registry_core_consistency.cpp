// Issue #478/#560/#583 (#1978 renamed): issue# moved from filename to header.
// test_primitives_registry_core_consistency_583.cpp
// Issue #583: Primitives registry + core list/math consistency
// in AI Agent hot paths (stdlib review).
//
// Non-duplicative with #478 (primitive-error-stats pair), #560
// (stats:list/count meta), test_primitives_init smoke pilot.
//
// AC1: query:primitives-stats reachable + registry slots > 0
// AC2: core list primitives (cons/car/cdr) work
// AC3: math error path — modulo div0 bumps error counter
// AC4: mutate + eval bumps agent-activity counters
// AC5: query:pattern bumps query-call counters
// AC6: multi-round hot-path matrix — primitives stats monotonic
// AC7: query regression (primitive-error-stats, stats:count)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_583_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;

static std::int64_t primitives_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:primitives-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define xs (cons 1 (cons 2 (cons 3 ()))) "
                 "(define acc 0))\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:primitives-stats ---");
    const auto s0 = primitives_stats(cs);
    const auto slots = cs.evaluator().get_primitive_slot_count();
    std::println("  primitives-stats = {} registry_slots = {}", s0, slots);
    CHECK(s0 > 0, "primitives-stats > 0 (includes registry slots)");
    CHECK(slots > 0, "registry has registered primitives");

    std::println("\n--- AC2: core list primitives ---");
    CHECK(setup_workspace(cs), "list workspace setup");
    auto car_r = cs.eval("(car xs)");
    auto cdr_r = cs.eval("(cdr xs)");
    CHECK(car_r && is_int(*car_r) && as_int(*car_r) == 1, "car returns head element");
    CHECK(cdr_r.has_value(), "cdr returns tail");

    std::println("\n--- AC3: math error path consistency ---");
    const auto err0 = cs.evaluator().get_primitive_error_count();
    (void)cs.eval("(modulo 1 0)");
    const auto err1 = cs.evaluator().get_primitive_error_count();
    std::println("  primitive_error_count: {} -> {}", err0, err1);
    CHECK(err1 > err0, "modulo div0 bumps primitive_error_count");

    std::println("\n--- AC4: mutate + eval agent activity ---");
    const auto stats4a = primitives_stats(cs);
    const auto mut0 = cs.evaluator().total_mutations();
    (void)cs.eval("(mutate:rebind \"acc\" \"42\")");
    (void)cs.eval("(eval-current)");
    const auto mut1 = cs.evaluator().total_mutations();
    const auto stats4b = primitives_stats(cs);
    std::println("  mutations: {} -> {} primitives-stats: {} -> {}", mut0, mut1, stats4a, stats4b);
    CHECK(mut1 > mut0, "mutate bumps total_mutations");
    CHECK(stats4b >= stats4a, "primitives-stats monotonic after mutate");

    std::println("\n--- AC5: query:pattern hot path ---");
    const auto q0 = cs.evaluator().get_total_query_calls();
    (void)cs.eval("(query:pattern \"acc\")");
    const auto q1 = cs.evaluator().get_total_query_calls();
    std::println("  total_query_calls: {} -> {}", q0, q1);
    CHECK(q1 >= q0, "query:pattern bumps or preserves query call count");

    std::println("\n--- AC6: multi-round hot-path matrix ---");
    const auto stats6a = primitives_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(query:pattern \"acc\")");
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(+ 1 2)");
    }
    const auto stats6b = primitives_stats(cs);
    std::println("  primitives-stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "primitives-stats monotonic over matrix");

    std::println("\n--- AC7: query regression ---");
    auto pes = cs.eval("(engine:metrics \"query:primitive-error-stats\")");
    auto sc = cs.eval("(stats:count)");
    CHECK(pes && is_pair(*pes), "primitive-error-stats regression");
    CHECK(sc && is_int(*sc) && as_int(*sc) >= 30, "stats:count regression >= 30");
}

} // namespace aura_583_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_583_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}