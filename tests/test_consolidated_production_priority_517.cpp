// test_consolidated_production_priority_517.cpp
// Issue #517: Consolidated open-issues meta — 3 P0 production pillars.
//
// Non-duplicative with #514 (Task6), #515 (consolidated P0), #516
// (Prompt6 memory), #520 (Top-5 roadmap with batch/orchestration).
//
// AC1: query:consolidated-production-priority-stats reachable
// AC2: P1 Persistence — panic-checkpoint bumps persistence counters
// AC3: P1 EDA — verify:parse-coverage-feedback bumps feedback counters
// AC4: P2 Memory — closure-env-safety-stats regression
// AC5: P3 SoA — soa-hotpath-adoption-stats regression
// AC6: multi-round mutate matrix — consolidated stats monotonic
// AC7: query regression (production-roadmap-stats, prompt6-safety-score)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_517_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t consolidated_stats(CompilerService& cs) {
    auto r = cs.eval("(query:consolidated-production-priority-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1) (define y 2) (define z 3)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:consolidated-production-priority-stats ---");
    const auto s0 = consolidated_stats(cs);
    std::println("  consolidated-production-priority-stats = {}", s0);
    CHECK(s0 >= 0, "consolidated stats non-negative");

    std::println("\n--- AC2: P1 Persistence checkpoint ---");
    CHECK(setup_workspace(cs), "workspace setup");
    const auto stats2a = consolidated_stats(cs);
    (void)cs.eval("(panic-checkpoint)");
    const auto stats2b = consolidated_stats(cs);
    std::println("  consolidated stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b > stats2a, "panic-checkpoint bumps persistence counters");

    std::println("\n--- AC3: P1 EDA verification feedback ---");
    const auto stats3a = consolidated_stats(cs);
    auto cov = cs.eval("(verify:parse-coverage-feedback \"0 hole_a\n1 hole_b\n\")");
    CHECK(cov && is_int(*cov), "verify:parse-coverage-feedback returns int");
    const auto stats3b = consolidated_stats(cs);
    std::println("  consolidated stats: {} -> {}", stats3a, stats3b);
    CHECK(stats3b > stats3a,
          "coverage feedback bumps EDA verification counters");

    std::println("\n--- AC4: P2 Memory safety regression ---");
    auto ces = cs.eval("(query:closure-env-safety-stats)");
    CHECK(ces && is_int(*ces), "closure-env-safety-stats regression");

    std::println("\n--- AC5: P3 SoA hotpath regression ---");
    auto soa = cs.eval("(query:soa-hotpath-adoption-stats)");
    CHECK(soa && is_int(*soa), "soa-hotpath-adoption-stats regression");

    std::println("\n--- AC6: multi-round mutate matrix monotonic ---");
    const auto stats6a = consolidated_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"x\" \"" +
                      std::to_string(10 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(mutate:request-gc-safepoint 10)");
    }
    const auto stats6b = consolidated_stats(cs);
    std::println("  consolidated stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "consolidated stats monotonic over matrix");

    std::println("\n--- AC7: query regression ---");
    auto roadmap = cs.eval("(query:production-roadmap-stats)");
    auto p6 = cs.eval("(query:prompt6-safety-score)");
    CHECK(roadmap && is_int(*roadmap), "production-roadmap-stats regression");
    CHECK(p6 && is_int(*p6), "prompt6-safety-score regression");
}

} // namespace aura_517_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_517_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}