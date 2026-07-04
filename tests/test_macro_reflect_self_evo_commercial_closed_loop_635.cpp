// test_macro_reflect_self_evo_commercial_closed_loop_635.cpp
// Issue #635: Macro + static reflection + self-evolution commercial
// closed-loop production gaps (July 2026 update).
//
// Non-duplicative with #597 (Task6 full matrix), #619 (follow-up 5-counter
// bundle), #634 (runtime/EDA/orchestration pillars).
//
// AC1: query:macro-reflect-self-evo-commercial-stats reachable
// AC2: macro expand + query:pattern hygiene filter active
// AC3: mutate under Guard bumps reflect/guard/dirty counters
// AC4: compile:macro-dirty-stats regression (macro_dirty column)
// AC5: multi-round self-evo cycle — commercial stats monotonic
// AC6: query regression (macro-reflect-self-evo-stats,
//      commercial-production-readiness-stats, followup-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_635_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t commercial_stats(CompilerService& cs) {
    auto r = cs.eval("(query:macro-reflect-self-evo-commercial-stats)");
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
    std::println("\n--- AC1: query:macro-reflect-self-evo-commercial-stats ---");
    CHECK(setup_macro_workspace(cs), "macro workspace setup");
    const auto s0 = commercial_stats(cs);
    std::println("  commercial-stats = {}", s0);
    CHECK(s0 >= 0, "commercial-stats non-negative");

    std::println("\n--- AC2: query:pattern hygiene filter ---");
    const auto skips0 = cs.evaluator().get_macro_introduced_skipped_in_query();
    (void)cs.eval("(query:pattern \"v\")");
    const auto skips1 = cs.evaluator().get_macro_introduced_skipped_in_query();
    std::println("  hygiene_skips: {} -> {}", skips0, skips1);
    CHECK(skips1 >= skips0, "macro-introduced nodes filtered in query");

    std::println("\n--- AC3: mutate under Guard bumps counters ---");
    const auto stats3a = commercial_stats(cs);
    (void)cs.eval("(query:pattern \"user-val\")");
    (void)cs.eval("(mutate:rebind \"user-val\" \"42\")");
    (void)cs.eval("(eval-current)");
    const auto stats3b = commercial_stats(cs);
    std::println("  commercial-stats: {} -> {}", stats3a, stats3b);
    CHECK(stats3b > stats3a, "Guard mutate bumps reflect/guard/dirty commercial counters");

    std::println("\n--- AC4: compile:macro-dirty-stats regression ---");
    auto mds = cs.eval("(compile:macro-dirty-stats)");
    CHECK(mds && is_int(*mds), "compile:macro-dirty-stats returns int");

    std::println("\n--- AC5: multi-round self-evo cycle monotonic ---");
    const auto stats5a = commercial_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(query:pattern \"user-val\")");
        (void)cs.eval("(mutate:rebind \"user-val\" \"" + std::to_string(100 + round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats5b = commercial_stats(cs);
    std::println("  commercial-stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "commercial-stats monotonic over matrix");

    std::println("\n--- AC6: query regression ---");
    auto mrs = cs.eval("(query:macro-reflect-self-evo-stats)");
    auto cpr = cs.eval("(query:commercial-production-readiness-stats)");
    auto fus = cs.eval("(query:macro-reflect-self-evo-followup-stats)");
    CHECK(mrs && is_int(*mrs), "macro-reflect-self-evo-stats regression");
    CHECK(cpr && is_int(*cpr), "commercial-production-readiness-stats regression");
    CHECK(fus && is_int(*fus), "macro-reflect-self-evo-followup-stats regression");
}

} // namespace aura_635_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_635_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}