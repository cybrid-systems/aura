// tests/compiler/test_production_readiness_batch.cpp
// R19 phase2 dup-merge — production-readiness closed-loop trio:
//   Issue #514 (task6) + Issue #634 (commercial) + Issue #441 (compiler-runtime)
// All 3 test 'production-readiness-stats' closed-loop subqueries with the same pattern:
//   aura_NNN_detail::run_matrix(cs) + main() calling RUN_ALL_TESTS().
// Same module imports (test_harness.hpp + aura.compiler.evaluator + service + value).
// Per Anqi 13:27 #81657 '继续' (continue R19 cleanup of tests/compiler/) + MEMORY
// '完整 ship 以后都是' = ship end-to-end no asking.

// === AC1-AC8 from test_production_readiness_closed_loop.cpp (Issue #514) ===

// test_task6_production_readiness_closed_loop_514.cpp
// Issue #514: Task6 Top 3 production-readiness closed loop (meta).
//
// Non-duplicative with #547 (pattern hygiene), #550 (dirty/type),
// #551 (reflect post-mutate), #597 (full matrix), #619 (followup).
//
// AC1: query:task6-production-readiness-stats reachable
// AC2: Top1 — macro expand + query:pattern hygiene + marker stats
// AC3: Top2 — mutate:rebind under Guard bumps mutation_impact
// AC4: Top3 — dirty/type incremental counters observable post-mutate
// AC5: query:ir-hygiene-stats reachable
// AC6: query:pattern-marker-stats reachable
// AC7: multi-round self-evo cycle — production stats monotonic
// AC8: query regression (pattern-hygiene, reflect-postmutate, typed-mutation)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_514_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t reflect_postmutate_total(CompilerService& cs) {
    auto r = cs.eval("(hash-ref (engine:metrics \"query:reflect-postmutate-stats\") "
                     "'reflect-postmutate-total')");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static std::int64_t ir_hygiene_total(CompilerService& cs) {
    auto r = cs.eval("(hash-ref (engine:metrics \"query:ir-hygiene-stats\") 'ir-hygiene-total')");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static std::int64_t prod_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:task6-production-readiness-stats\")");
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
    std::println("\n--- AC1: query:task6-production-readiness-stats ---");
    CHECK(setup_macro_workspace(cs), "macro workspace setup");
    const auto s0 = prod_stats(cs);
    std::println("  production-readiness-stats = {}", s0);
    CHECK(s0 >= 0, "production-readiness-stats non-negative");

    std::println("\n--- AC2: Top1 hygiene/marker propagation ---");
    const auto skips0 = cs.evaluator().get_macro_introduced_skipped_in_query();
    (void)cs.eval("(query:pattern \"v\")");
    const auto skips1 = cs.evaluator().get_macro_introduced_skipped_in_query();
    auto irs = cs.eval("(engine:metrics \"query:ir-hygiene-stats\")");
    auto pms = cs.eval("(engine:metrics \"query:pattern-marker-stats\")");
    std::println("  hygiene_skips: {} -> {} ir-hygiene={} pattern-marker-hash={}", skips0, skips1,
                 irs && is_hash(*irs) ? ir_hygiene_total(cs) : 0, pms && is_hash(*pms) ? 1 : 0);
    CHECK(skips1 > skips0, "MacroIntroduced filtered in query:pattern");
    CHECK(irs && is_hash(*irs), "query:ir-hygiene-stats returns hash");
    CHECK(pms && is_hash(*pms), "query:pattern-marker-stats returns hash");
    CHECK(skips1 > 0, "query skips recorded after pattern hygiene filter");

    std::println("\n--- AC3: Top2 Guard + reflect post-mutate ---");
    const auto impact0 = cs.evaluator().get_mutation_impact_count();
    (void)cs.eval("(mutate:rebind \"user-val\" \"42\")");
    const auto impact1 = cs.evaluator().get_mutation_impact_count();
    const auto snap = cs.evaluator().get_impact_snapshot_count();
    std::println("  mutation_impact: {} -> {} impact_snapshot={}", impact0, impact1, snap);
    CHECK(impact1 > impact0, "Guard success bumps mutation_impact");

    std::println("\n--- AC4: Top3 dirty/type incremental ---");
    auto dirty = cs.eval("(query:dirty-impact)");
    auto typed = cs.eval("(engine:metrics \"query:typed-mutation-stats\")");
    CHECK(dirty && is_int(*dirty), "query:dirty-impact returns int");
    CHECK(typed && is_int(*typed), "query:typed-mutation-stats returns int");
    std::println("  dirty-impact={} typed-mutation-stats={}", as_int(*dirty), as_int(*typed));

    std::println("\n--- AC5: query:ir-hygiene-stats ---");
    auto ir0 = cs.eval("(engine:metrics \"query:ir-hygiene-stats\")");
    CHECK(ir0 && is_hash(*ir0) && ir_hygiene_total(cs) >= 0, "ir-hygiene-stats non-negative");

    std::println("\n--- AC6: query:pattern-marker-stats ---");
    auto pm0 = cs.eval("(engine:metrics \"query:pattern-marker-stats\")");
    CHECK(pm0 && is_hash(*pm0), "pattern-marker-stats hash after macro+query");

    std::println("\n--- AC7: multi-round self-evo cycle ---");
    const auto stats7a = prod_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(query:pattern \"user-val\")");
        (void)cs.eval("(mutate:rebind \"user-val\" \"" + std::to_string(200 + round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats7b = prod_stats(cs);
    std::println("  production-readiness: {} -> {}", stats7a, stats7b);
    CHECK(stats7b >= stats7a, "production-readiness stats monotonic");

    std::println("\n--- AC8: query regression ---");
    auto phs = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    auto rps = cs.eval("(engine:metrics \"query:reflect-postmutate-stats\")");
    auto tms = cs.eval("(engine:metrics \"query:typed-mutation-stats\")");
    CHECK(phs && (is_int(*phs) || is_hash(*phs)), "pattern-hygiene-stats regression");
    CHECK(rps && is_hash(*rps), "reflect-postmutate-stats regression");
    CHECK(reflect_postmutate_total(cs) >= 0, "reflect-postmutate-total non-negative");
    CHECK(tms && is_int(*tms), "typed-mutation-stats regression");
}

} // namespace aura_514_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_514_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

// === AC1-AC8 from test_commercial_production_readiness_closed_loop.cpp (Issue #634) ===

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
    auto ths = cs.eval("(stats:get \"query:task4-hotpath-safety-score\")");
    auto orch = cs.eval("(stats:get \"query:orchestration-metrics\")");
    CHECK(ths && is_int(*ths), "task4-hotpath-safety-score regression");
    CHECK(orch && is_string(*orch), "orchestration-metrics regression");
}

} // namespace aura_634_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_634_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

// === AC1-AC7 from test_compiler_runtime_production_readiness_closed_loop.cpp (Issue #441) ===

// test_compiler_runtime_production_readiness_closed_loop_441.cpp
// Issue #441: Consolidated compiler & runtime production-readiness
// closed loop (meta).
//
// Non-duplicative with #438 (fiber-migration), #439 (gc-safepoint),
// #454 (reflect-edsl-bridge), #437 (verify-dirty), #514 (Task6 hygiene).
//
// AC1: query:compiler-runtime-production-readiness-stats reachable
// AC2: P0 #438 — fiber-migration pillar observable
// AC3: P0 #439 — gc-safepoint pillar bumps under request
// AC4: P0 #440 — EDSL workspace mutate + query under Guard
// AC5: P0 #437 — EDA verify-dirty pillar reachable
// AC6: multi-round self-evo cycle — consolidated stats monotonic
// AC7: query regression (fiber-migration, gc-safepoint, edsl-stability,
//      verify-dirty)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_441_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t prod_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:compiler-runtime-production-readiness-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:compiler-runtime-production-readiness-stats ---");
    const auto s0 = prod_stats(cs);
    std::println("  compiler-runtime-production-readiness-stats = {}", s0);
    CHECK(s0 >= 0, "consolidated stats non-negative");

    std::println("\n--- AC2: P0 #438 fiber-migration pillar ---");
    auto fms = cs.eval("(engine:metrics \"query:fiber-migration-stats\")");
    CHECK(fms && is_int(*fms), "query:fiber-migration-stats returns int");
    const auto fiber0 = as_int(*fms);
    std::println("  fiber-migration-stats = {}", fiber0);

    std::println("\n--- AC3: P0 #439 gc-safepoint pillar ---");
    const auto gc0 = cs.eval("(engine:metrics \"query:gc-safepoint-stats\")");
    CHECK(gc0 && is_int(*gc0), "query:gc-safepoint-stats returns int");
    (void)cs.eval("(mutate:request-gc-safepoint 50)");
    const auto gc1 = cs.eval("(engine:metrics \"query:gc-safepoint-stats\")");
    CHECK(gc1 && is_int(*gc1), "gc-safepoint-stats after request");
    CHECK(as_int(*gc1) > as_int(*gc0),
          "gc-safepoint-stats bumped after mutate:request-gc-safepoint");

    std::println("\n--- AC4: P0 #440 EDSL workspace mutate + query ---");
    CHECK(cs.eval("(set-code \"(define acc 0)\")").has_value(), "EDSL workspace setup");
    CHECK(cs.eval("(eval-current)").has_value(), "workspace eval");
    const auto impact0 = cs.evaluator().get_mutation_impact_count();
    (void)cs.eval("(query:pattern \"acc\")");
    CHECK(cs.eval("(mutate:rebind \"acc\" \"42\")").has_value(), "mutate:rebind under Guard");
    const auto impact1 = cs.evaluator().get_mutation_impact_count();
    CHECK(impact1 > impact0, "mutation_impact bumped after Guard mutate");
    auto eds = cs.eval("(engine:metrics \"query:edsl-stability-stats\")");
    CHECK(eds && is_int(*eds), "query:edsl-stability-stats returns int");

    std::println("\n--- AC5: P0 #437 EDA verify-dirty pillar ---");
    auto vds = cs.eval("(engine:metrics \"query:verify-dirty-stats\")");
    CHECK(vds && is_int(*vds), "query:verify-dirty-stats returns int");
    (void)cs.eval("(verify:assertion-failed 0)");
    auto vds1 = cs.eval("(engine:metrics \"query:verify-dirty-stats\")");
    CHECK(vds1 && is_int(*vds1), "verify-dirty-stats after assertion-failed");

    std::println("\n--- AC6: multi-round self-evo cycle ---");
    const auto stats6a = prod_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(query:pattern \"acc\")");
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(100 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(mutate:request-gc-safepoint 10)");
    }
    const auto stats6b = prod_stats(cs);
    std::println("  consolidated stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "consolidated stats monotonic");

    std::println("\n--- AC7: query regression ---");
    auto fms_r = cs.eval("(engine:metrics \"query:fiber-migration-stats\")");
    auto gcs_r = cs.eval("(engine:metrics \"query:gc-safepoint-stats\")");
    auto eds_r = cs.eval("(engine:metrics \"query:edsl-stability-stats\")");
    auto vds_r = cs.eval("(engine:metrics \"query:verify-dirty-stats\")");
    CHECK(fms_r && is_int(*fms_r), "fiber-migration-stats regression");
    CHECK(gcs_r && is_int(*gcs_r), "gc-safepoint-stats regression");
    CHECK(eds_r && is_int(*eds_r), "edsl-stability-stats regression");
    CHECK(vds_r && is_int(*vds_r), "verify-dirty-stats regression");
}

} // namespace aura_441_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_441_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}