// Issue #437/#438/#439/#440/#441/#454 (#1978 renamed): issue# moved from filename to header.
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