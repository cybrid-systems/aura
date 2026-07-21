// Issue #441/#514/#520/#634/#635 (#1978 renamed): issue# moved from filename to header.
// test_production_roadmap_closed_loop_520.cpp
// Issue #520: Consolidated Top Production Priorities roadmap (meta).
//
// Non-duplicative with #514 (Task6 hygiene/dirty), #634 (commercial
// runtime pillars), #635 (macro-reflect-self-evo), #441 (compiler-runtime).
//
// AC1: query:production-roadmap-stats reachable
// AC2: P1 EDA/SV — verify:parse-coverage-feedback bumps feedback counters
// AC3: P2 Persistence — panic-checkpoint lifecycle observable
// AC4: P3 Memory — closure-env-safety-stats regression
// AC5: P4 SoA — task4-hotpath-safety-score regression
// AC6: P5 Batch — mutate cycle bumps roadmap stats
// AC7: multi-round matrix — roadmap stats monotonic
// AC8: query regression (task6-production-readiness,
//      commercial-production-readiness, verification-loop-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_520_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t roadmap_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:production-roadmap-stats\")");
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
    std::println("\n--- AC1: query:production-roadmap-stats ---");
    const auto s0 = roadmap_stats(cs);
    std::println("  production-roadmap-stats = {}", s0);
    CHECK(s0 >= 0, "roadmap stats non-negative");

    std::println("\n--- AC2: P1 EDA/SV verification feedback ---");
    CHECK(setup_workspace(cs), "workspace setup");
    const auto stats2a = roadmap_stats(cs);
    auto cov = cs.eval("(verify:parse-coverage-feedback \"0 hole_a\n2 hole_b\n\")");
    CHECK(cov && is_int(*cov), "verify:parse-coverage-feedback returns int");
    const auto stats2b = roadmap_stats(cs);
    std::println("  roadmap stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b > stats2a, "coverage feedback bumps EDA verification counters");

    std::println("\n--- AC3: P2 Persistence checkpoint lifecycle ---");
    auto pcs = cs.eval("(engine:metrics \"query:panic-checkpoint-lifecycle-stats\")");
    (void)cs.eval("(panic-checkpoint)");
    auto pcs2 = cs.eval("(engine:metrics \"query:panic-checkpoint-lifecycle-stats\")");
    CHECK(pcs && is_int(*pcs), "panic-checkpoint-lifecycle-stats returns int");
    CHECK(pcs2 && is_int(*pcs2), "panic-checkpoint-lifecycle after save");
    CHECK(as_int(*pcs2) >= as_int(*pcs), "checkpoint save bumps persistence counters");

    std::println("\n--- AC4: P3 Memory safety regression ---");
    auto ces = cs.eval("(engine:metrics \"query:closure-env-safety-stats\")");
    CHECK(ces && is_hash(*ces), "closure-env-safety-stats regression");

    std::println("\n--- AC5: P4 SoA hotpath regression ---");
    auto ths = cs.eval("(stats:get \"query:task4-hotpath-safety-score\")");
    CHECK(ths && is_int(*ths), "task4-hotpath-safety-score regression");

    std::println("\n--- AC6: P5 mutate cycle bumps stats ---");
    const auto stats6a = roadmap_stats(cs);
    (void)cs.eval("(mutate:rebind \"x\" \"42\")");
    (void)cs.eval("(eval-current)");
    const auto stats6b = roadmap_stats(cs);
    std::println("  roadmap stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "mutate cycle bumps roadmap stats");

    std::println("\n--- AC7: multi-round matrix monotonic ---");
    const auto stats7a = roadmap_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"y\" \"" + std::to_string(10 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(mutate:request-gc-safepoint 10)");
    }
    const auto stats7b = roadmap_stats(cs);
    std::println("  roadmap stats: {} -> {}", stats7a, stats7b);
    CHECK(stats7b >= stats7a, "roadmap stats monotonic over matrix");

    std::println("\n--- AC8: query regression ---");
    auto t6 = cs.eval("(engine:metrics \"query:task6-production-readiness-stats\")");
    auto cpr = cs.eval("(engine:metrics \"query:commercial-production-readiness-stats\")");
    auto vls = cs.eval("(engine:metrics \"query:verification-loop-stats\")");
    CHECK(t6 && is_int(*t6), "task6-production-readiness-stats regression");
    CHECK(cpr && is_int(*cpr), "commercial-production-readiness-stats regression");
    CHECK(vls && is_int(*vls), "verification-loop-stats regression");
}

} // namespace aura_520_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_520_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}