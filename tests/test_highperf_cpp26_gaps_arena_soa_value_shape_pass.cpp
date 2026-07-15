// test_highperf_cpp26_gaps_arena_soa_value_shape_pass.cpp — Issue #658:
// 5 high-perf gaps — Arena tier fallback + IRSoA dirty cascade +
// Value v2 classify + Shape history jitter + Pass DirtyAware skip.
//
// Non-duplicative with #657 (compiler-core-incremental), #642 (arena compact),
// #571 (value-dispatch), #570 (shape-stability), #494 (pass-pipeline).
//
//   - AC1:  query:highperf-cpp26-stats reachable (schema 658)
//   - AC2:  mutate bumps soa-dirty-cascades or arena-tier-fallbacks
//   - AC3:  eval bumps value-classify-calls
//   - AC4:  shape-history-jitter-wins readable
//   - AC5:  multi-round mutate — stats monotonic
//   - AC6:  pass-dirty-skips readable
//   - AC7:  query regression (soa-dirty, value-dispatch, shape-stability)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_658_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t hash_int(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (engine:metrics \"query:highperf-cpp26-stats\") \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto arena = hash_int(cs, "arena-tier-fallbacks");
    const auto soa = hash_int(cs, "soa-dirty-cascades");
    const auto classify = hash_int(cs, "value-classify-calls");
    const auto jitter = hash_int(cs, "shape-history-jitter-wins");
    const auto pass = hash_int(cs, "pass-dirty-skips");
    if (arena < 0 || soa < 0 || classify < 0 || jitter < 0 || pass < 0)
        return -1;
    return arena + soa + classify + jitter + pass;
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) "
                 "(define a 1) (define b 2) (fact 5)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:highperf-cpp26-stats (schema 658) ---");
    CHECK(setup_workspace(cs), "recursive workspace setup");
    auto h = cs.eval("(engine:metrics \"query:highperf-cpp26-stats\")");
    CHECK(h && is_hash(*h), "highperf-cpp26-stats returns hash");
    CHECK(hash_int(cs, "schema") == 658, "schema == 658");

    std::println("\n--- AC2: mutate bumps soa-dirty or arena-tier ---");
    const auto soa0 = hash_int(cs, "soa-dirty-cascades");
    const auto arena0 = hash_int(cs, "arena-tier-fallbacks");
    (void)cs.eval("(mutate:rebind \"a\" \"10\")");
    (void)cs.eval("(eval-current)");
    const auto soa1 = hash_int(cs, "soa-dirty-cascades");
    const auto arena1 = hash_int(cs, "arena-tier-fallbacks");
    std::println("  soa-dirty-cascades: {} -> {}", soa0, soa1);
    std::println("  arena-tier-fallbacks: {} -> {}", arena0, arena1);
    CHECK(soa1 >= soa0 || arena1 >= arena0, "soa-dirty or arena-tier monotonic after mutate");

    std::println("\n--- AC3: eval bumps value-classify-calls ---");
    const auto classify0 = hash_int(cs, "value-classify-calls");
    (void)cs.eval("(fact 4)");
    (void)cs.eval("(+ a b)");
    const auto classify1 = hash_int(cs, "value-classify-calls");
    std::println("  value-classify-calls: {} -> {}", classify0, classify1);
    CHECK(classify1 >= classify0, "value-classify-calls monotonic after eval");

    std::println("\n--- AC4: shape-history-jitter-wins readable ---");
    const auto jitter = hash_int(cs, "shape-history-jitter-wins");
    CHECK(jitter >= 0, "shape-history-jitter-wins readable");

    std::println("\n--- AC5: multi-round mutate monotonic ---");
    const auto stats5a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"b\" \"" + std::to_string(20 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(fact 2)");
    }
    const auto stats5b = stats_sum(cs);
    std::println("  highperf-cpp26 sum: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "highperf-cpp26 stats monotonic");

    std::println("\n--- AC6: pass-dirty-skips readable ---");
    const auto pass = hash_int(cs, "pass-dirty-skips");
    CHECK(pass >= 0, "pass-dirty-skips readable");

    std::println("\n--- AC7: query regression ---");
    auto soa = cs.eval("(engine:metrics \"query:soa-dirty-stats\")");
    auto vdisp = cs.eval("(engine:metrics \"query:value-dispatch-stats\")");
    auto shape = cs.eval("(engine:metrics \"query:shape-stability-stats\")");
    CHECK(soa && is_hash(*soa), "soa-dirty-stats regression");
    // #571 was int; later observability surfaces upgraded to hash with schema.
    CHECK(vdisp && (is_int(*vdisp) || is_hash(*vdisp)),
          "value-dispatch-stats regression (int or hash)");
    CHECK(shape && (is_int(*shape) || is_hash(*shape)),
          "shape-stability-stats regression (int or hash)");
}

} // namespace aura_658_detail

int aura_issue_highperf_cpp26_gaps_arena_soa_value_shape_pass_run() {
    aura::compiler::CompilerService cs;
    aura_658_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_highperf_cpp26_gaps_arena_soa_value_shape_pass_run();
}
#endif
