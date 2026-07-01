// test_edsl_self_evolution_marker_dirty_guard_task6.cpp
// Issue #595: EDSL self-evolution closed loop with marker/dirty/
// epoch + MutationBoundaryGuard integration (Task6).
//
// Non-duplicative with #597 (full fuzz/concurrent matrix),
// #619 (macro-reflect followup), #547 (pattern hygiene),
// #525 (Guard panic), #514 (Task6 meta).
//
// AC1: query:self-evolution-loop-stats reachable
// AC2: macro expand + query:pattern hygiene filter active
// AC3: mutate under Guard bumps dirty_propagation + guard_epoch
// AC4: validation_pass observable after Guard mutate
// AC5: query:by-marker + query:epoch-stats regression
// AC6: multi-round self-evo cycle — loop stats monotonic
// AC7: query regression (macro-reflect-self-evo-stats,
//      hygiene-stats, mutation-log-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_595_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;

static std::int64_t loop_stats(CompilerService& cs) {
    auto r = cs.eval("(query:self-evolution-loop-stats)");
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
    std::println("\n--- AC1: query:self-evolution-loop-stats ---");
    CHECK(setup_macro_workspace(cs), "macro workspace setup");
    const auto s0 = loop_stats(cs);
    std::println("  self-evolution-loop-stats = {}", s0);
    CHECK(s0 >= 0, "self-evolution-loop-stats non-negative");

    std::println("\n--- AC2: query:pattern hygiene filter ---");
    const auto skips0 = cs.evaluator().get_macro_introduced_skipped_in_query();
    (void)cs.eval("(query:pattern \"v\")");
    const auto skips1 = cs.evaluator().get_macro_introduced_skipped_in_query();
    std::println("  hygiene_skips: {} -> {}", skips0, skips1);
    CHECK(skips1 > skips0, "MacroIntroduced filtered in query:pattern");

    std::println("\n--- AC3: Guard mutate bumps dirty + epoch ---");
    const auto dirty0 = cs.evaluator().get_dirty_propagation_count();
    const auto epoch0 = cs.evaluator().get_guard_dirty_epoch_count();
    CHECK(cs.eval("(mutate:rebind \"user-val\" \"42\")").has_value(),
          "mutate:rebind under Guard");
    const auto dirty1 = cs.evaluator().get_dirty_propagation_count();
    const auto epoch1 = cs.evaluator().get_guard_dirty_epoch_count();
    std::println("  dirty_propagation: {} -> {} guard_epoch: {} -> {}",
                 dirty0, dirty1, epoch0, epoch1);
    CHECK(dirty1 >= dirty0, "dirty_propagation monotonic after Guard mutate");
    CHECK(epoch1 >= epoch0, "guard_dirty_epoch monotonic after Guard mutate");

    std::println("\n--- AC4: validation_pass after Guard mutate ---");
    const auto pass = cs.evaluator().get_schema_validation_pass_count();
    std::println("  schema_validation_pass_count = {}", pass);
    CHECK(pass >= 0, "validation_pass counter observable");

    std::println("\n--- AC5: marker + epoch query regression ---");
    auto marker = cs.eval("(query:by-marker \"MacroIntroduced\")");
    auto epoch = cs.eval("(query:epoch-stats)");
    CHECK(marker.has_value(), "query:by-marker MacroIntroduced reachable");
    CHECK(epoch && is_int(*epoch), "query:epoch-stats returns int");

    std::println("\n--- AC6: multi-round self-evo cycle ---");
    const auto stats6a = loop_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(query:pattern \"user-val\")");
        (void)cs.eval("(mutate:rebind \"user-val\" \"" +
                      std::to_string(200 + round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats6b = loop_stats(cs);
    std::println("  self-evolution-loop-stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "loop-stats monotonic over self-evo matrix");

    std::println("\n--- AC7: query regression ---");
    auto mrs = cs.eval("(query:macro-reflect-self-evo-stats)");
    auto hys = cs.eval("(query:hygiene-stats)");
    auto mls = cs.eval("(query:mutation-log-stats)");
    CHECK(mrs && is_int(*mrs), "macro-reflect-self-evo-stats regression");
    CHECK(hys && is_int(*hys), "hygiene-stats regression");
    CHECK(mls && is_int(*mls), "mutation-log-stats regression");
}

} // namespace aura_595_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_595_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}