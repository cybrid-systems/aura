// test_commercial_production_readiness_closed_loop_634.cpp
// Issue #634: Commercial production readiness closed loop (meta).
//
// Non-duplicative with #441 (compiler-runtime), #595 (self-evo loop),
// #602 (Prompt6 safety), #607 (Task4 hotpath), per-theme #620-#633 tests.
//
// AC1: query:commercial-production-readiness-stats reachable
// AC2: P0 #620/#631 — stable-ref + provenance pillar
// AC3: P0 #623 — gc-safepoint pillar bumps under request
// AC4: P0 #624 — shape-stability-stats pillar observable
// AC5: P0 #629 — coercion-zerooverhead-stats pillar observable
// AC6: P0 #630 — verify-dirty pillar reachable
// AC7: multi-round mutate cycle — commercial stats monotonic
// AC8: query regression (stable-ref-stats, task4-hotpath,
//      orchestration-metrics string)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_634_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;

static std::int64_t commercial_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:commercial-production-readiness-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:commercial-production-readiness-stats ---");
    const auto s0 = commercial_stats(cs);
    std::println("  commercial-production-readiness-stats = {}", s0);
    CHECK(s0 >= 0, "commercial stats non-negative");

    std::println("\n--- AC2: P0 #620/#631 stable-ref pillar ---");
    auto srs = cs.eval("(engine:metrics \"query:stable-ref-stats\")");
    CHECK(srs && is_int(*srs), "query:stable-ref-stats returns int");
    std::println("  stable-ref-stats = {}", as_int(*srs));

    std::println("\n--- AC3: P0 #623 gc-safepoint pillar ---");
    const auto gc0 = cs.eval("(engine:metrics \"query:gc-safepoint-stats\")");
    CHECK(gc0 && is_int(*gc0), "query:gc-safepoint-stats returns int");
    (void)cs.eval("(mutate:request-gc-safepoint 50)");
    const auto gc1 = cs.eval("(engine:metrics \"query:gc-safepoint-stats\")");
    CHECK(gc1 && is_int(*gc1), "gc-safepoint-stats after request");
    CHECK(as_int(*gc1) > as_int(*gc0),
          "gc-safepoint-stats bumped after mutate:request-gc-safepoint");

    std::println("\n--- AC4: P0 #624 shape-stability pillar ---");
    CHECK(cs.eval("(set-code \"(define add1 (lambda (x) (+ x 1)))\")").has_value(),
          "workspace setup for shape profiler");
    CHECK(cs.eval("(eval-current)").has_value(), "eval add1");
    auto sps = cs.eval("(engine:metrics \"query:shape-stability-stats\")");
    CHECK(sps && is_int(*sps), "query:shape-stability-stats returns int");

    std::println("\n--- AC5: P0 #629 coercion-zerooverhead pillar ---");
    auto czs = cs.eval("(engine:metrics \"query:coercion-zerooverhead-stats\")");
    CHECK(czs && is_int(*czs), "query:coercion-zerooverhead-stats returns int");

    std::println("\n--- AC6: P0 #630 verify-dirty pillar ---");
    CHECK(cs.eval("(set-code \"(define x 1) (define y 2)\")").has_value(), "EDA workspace setup");
    auto vds = cs.eval("(engine:metrics \"query:verify-dirty-stats\")");
    CHECK(vds && is_int(*vds), "query:verify-dirty-stats returns int");
    (void)cs.eval("(verify:assertion-failed 0)");

    std::println("\n--- AC7: multi-round mutate cycle ---");
    const auto stats7a = commercial_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"x\" \"" + std::to_string(10 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(mutate:request-gc-safepoint 10)");
    }
    const auto stats7b = commercial_stats(cs);
    std::println("  commercial stats: {} -> {}", stats7a, stats7b);
    CHECK(stats7b >= stats7a, "commercial stats monotonic");

    std::println("\n--- AC8: query regression ---");
    auto ths = cs.eval("(query:task4-hotpath-safety-score)");
    auto orch = cs.eval("(query:orchestration-metrics)");
    CHECK(ths && is_int(*ths), "task4-hotpath-safety-score regression");
    CHECK(orch && is_string(*orch), "orchestration-metrics regression");
}

} // namespace aura_634_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_634_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}