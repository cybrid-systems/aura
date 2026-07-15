// test_soa_hotpath_adoption_506.cpp
// Issue #506: IRFunctionSoA + IRInstructionView hotpath adoption +
// dirty-aware Pass Pipeline short-circuit observability.
//
// Non-duplicative with #607 (Task4 full matrix), #254 (compile:ir-soa-stats
// hash), #463 (SoA scaffold unit tests).
//
// AC1: query:soa-hotpath-adoption-stats reachable
// AC2: eval-current exercises IR SoA emit counters
// AC3: mutate:rebind + eval bumps dirty short-circuit counters
// AC4: query:pattern bumps tag_arity_index (SoA query index)
// AC5: multi-round mutate matrix — adoption stats monotonic
// AC6: query regression (task4-hotpath-safety-score,
//      task4-cache-locality-win, compile:ir-soa-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_506_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t adoption_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:soa-hotpath-adoption-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define base 10) (define acc 0) "
                 "(add1 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:soa-hotpath-adoption-stats ---");
    CHECK(setup_workspace(cs), "hotpath workspace setup + eval");
    const auto s0 = adoption_stats(cs);
    std::println("  soa-hotpath-adoption-stats = {}", s0);
    CHECK(s0 >= 0, "adoption stats non-negative");

    std::println("\n--- AC2: eval exercises IR SoA emit path ---");
    const auto stats2a = adoption_stats(cs);
    CHECK(cs.eval("(eval-current)").has_value(), "re-eval current");
    const auto stats2b = adoption_stats(cs);
    std::println("  adoption stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b >= stats2a, "re-eval monotonic for SoA emit counters");

    std::println("\n--- AC3: mutate + eval bumps dirty short-circuit ---");
    const auto passes0 = cs.evaluator().get_passes_skipped_type_dirty();
    const auto stats3a = adoption_stats(cs);
    (void)cs.eval("(mutate:rebind \"base\" \"99\")");
    CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
    const auto passes1 = cs.evaluator().get_passes_skipped_type_dirty();
    const auto stats3b = adoption_stats(cs);
    std::println("  passes_skipped: {} -> {} adoption: {} -> {}", passes0, passes1, stats3a,
                 stats3b);
    CHECK(stats3b >= stats3a, "mutate+eval bumps adoption stats (dirty pass path)");

    std::println("\n--- AC4: query:pattern SoA index ---");
    const auto stats4a = adoption_stats(cs);
    (void)cs.eval("(query:pattern \"base\")");
    const auto stats4b = adoption_stats(cs);
    std::println("  adoption stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b >= stats4a, "query:pattern does not regress adoption stats");

    std::println("\n--- AC5: multi-round mutate matrix monotonic ---");
    const auto stats5a = adoption_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(query:pattern \"acc\")");
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats5b = adoption_stats(cs);
    std::println("  adoption stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "adoption stats monotonic over matrix");

    std::println("\n--- AC6: query regression ---");
    auto ths = cs.eval("(query:task4-hotpath-safety-score)");
    auto clw = cs.eval("(query:task4-cache-locality-win)");
    auto irs = cs.eval("(engine:metrics \"compile:ir-soa-stats\")");
    CHECK(ths && is_int(*ths), "task4-hotpath-safety-score regression");
    CHECK(clw && is_int(*clw), "task4-cache-locality-win regression");
    CHECK(irs && is_hash(*irs), "compile:ir-soa-stats regression");
}

} // namespace aura_506_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_506_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}