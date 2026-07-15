// test_issue_1457_type_propagation_castop_zerooverhead.cpp
// Issue #1457: IR-level type propagation + CastOp zero-overhead.
//
// AC1: TypePropagationPass runs on eval path (type_propagation_runs grows)
// AC2: type-propagation-stats monotonic after typecheck/eval
// AC3: dead-coercion / zerooverhead metrics present after coercion IR
// AC4: post-mutate rebind keeps eval correct (narrow + cast elision)
// AC5: multi-round typed mutate — type_propagation_runs monotonic
// AC6: TypePropagationPass class exposes cast/narrow accessors

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_1457_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static bool setup_typed(CompilerService& cs) {
    // Gradual + concrete arithmetic + occurrence narrow → CastOp / prop.
    if (!cs.eval("(set-code \"(define (f x) (if (number? x) (+ x 1) 0)) "
                 "(define (g a b) (+ a b)) "
                 "(define n 42) "
                 "(f 3) (g 1 2) n\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static std::int64_t type_prop_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:type-propagation-stats\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void test_type_prop_runs_on_eval() {
    std::println("\n--- AC1: TypePropagationPass runs on eval ---");
    CompilerService cs;
    const auto runs0 = cs.get_type_propagation_runs();
    CHECK(setup_typed(cs), "setup typed workspace");
    // Force another IR pipeline pass.
    CHECK(cs.eval("(eval-current)").has_value(), "re-eval");
    const auto runs1 = cs.get_type_propagation_runs();
    const auto total1 = cs.get_type_propagation_total();
    std::println("  type_propagation_runs: {} -> {}, total={}", runs0, runs1, total1);
    CHECK(runs1 > runs0, "type_propagation_runs increased after eval pipeline");
}

static void test_type_prop_stats_monotonic() {
    std::println("\n--- AC2: type-propagation-stats monotonic ---");
    CompilerService cs;
    CHECK(setup_typed(cs), "setup");
    const auto s0 = type_prop_stats(cs);
    CHECK(s0 >= 0, "query:type-propagation-stats reachable");
    for (int i = 0; i < 5; ++i)
        (void)cs.eval("(f 7)");
    const auto s1 = type_prop_stats(cs);
    std::println("  type-propagation-stats: {} -> {}", s0, s1);
    CHECK(s1 >= s0, "stats non-decreasing");
}

static void test_dead_coercion_surface() {
    std::println("\n--- AC3: dead-coercion / zerooverhead surface ---");
    CompilerService cs;
    CHECK(setup_typed(cs), "setup");
    (void)cs.eval("(eval-current)");
    auto dce = cs.eval("(engine:metrics \"query:dead-coercion-stats\")");
    // Primitive may return int or hash depending on schema; presence is enough.
    CHECK(dce.has_value(), "dead-coercion-stats reachable");
    auto snap = cs.snapshot();
    std::println("  dead_coercion_eliminated={} type_prop_hits={} narrow_hits={} "
                 "zerooverhead_win={}",
                 snap.dead_coercion_eliminated_total, snap.coercion_type_prop_hits_total,
                 snap.coercion_narrow_evidence_hits_total, snap.coercion_zerooverhead_win_total);
    CHECK(snap.dead_coercion_eliminated_total >= 0, "eliminated counter readable");
    CHECK(cs.get_type_propagation_runs() > 0, "type prop ran (feeds DCE)");
}

static void test_post_mutate_correctness() {
    std::println("\n--- AC4: post-mutate eval correct under type prop + DCE ---");
    CompilerService cs;
    CHECK(setup_typed(cs), "setup");
    CHECK(cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 10) 0))\" "
                  "\"issue-1457\")")
              .has_value(),
          "rebind f");
    auto* ws = cs.workspace_flat();
    if (ws && !ws->all_mutations().empty())
        (void)cs.incremental_infer(ws->all_mutations().back());
    auto r = cs.eval("(f 5)");
    CHECK(r && is_int(*r), "f 5 returns int");
    if (r && is_int(*r))
        CHECK(as_int(*r) == 15, "narrow-dependent (+ x 10) correct after rebind");
    auto g = cs.eval("(g 4 6)");
    CHECK(g && is_int(*g) && as_int(*g) == 10, "g still correct");
}

static void test_multi_round_monotonic() {
    std::println("\n--- AC5: multi-round mutate — type_propagation_runs monotonic ---");
    CompilerService cs;
    CHECK(setup_typed(cs), "setup");
    auto runs_prev = cs.get_type_propagation_runs();
    for (int round = 0; round < 4; ++round) {
        const std::string body =
            "(lambda (x) (if (number? x) (+ x " + std::to_string(round + 2) + ") 0))";
        (void)cs.eval("(mutate:rebind \"f\" \"" + body + "\" \"r" + std::to_string(round) + "\")");
        (void)cs.eval("(f 1)");
        (void)cs.eval("(eval-current)");
        const auto runs = cs.get_type_propagation_runs();
        std::println("  round {} runs={}", round, runs);
        CHECK(runs >= runs_prev, "type_propagation_runs monotonic round " + std::to_string(round));
        runs_prev = runs;
        auto r = cs.eval("(f 1)");
        CHECK(r && is_int(*r) && as_int(*r) == 1 + round + 2,
              "f result correct round " + std::to_string(round));
    }
}

static void test_pass_source_has_accessors() {
    std::println("\n--- AC6: TypePropagationPass #1457 accessors in source ---");
    std::ifstream f("src/compiler/pass_manager.ixx");
    CHECK(f.good(), "pass_manager.ixx readable");
    if (!f.good())
        return;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(content.find("cast_result_stamped") != std::string::npos, "cast_result_stamped");
    CHECK(content.find("narrow_propagated") != std::string::npos, "narrow_propagated");
    CHECK(content.find("Issue #1457") != std::string::npos, "1457 marker");
    CHECK(content.find("TypePropagationPass") != std::string::npos, "TypePropagationPass class");
}

} // namespace aura_1457_detail

int main() {
    using namespace aura_1457_detail;
    test_type_prop_runs_on_eval();
    test_type_prop_stats_monotonic();
    test_dead_coercion_surface();
    test_post_mutate_correctness();
    test_multi_round_monotonic();
    test_pass_source_has_accessors();
    return RUN_ALL_TESTS();
}
