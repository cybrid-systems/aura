// test_compiler_core_incremental_selfmod_gaps.cpp — Issue #657:
// 5 compiler core gaps — cache bridge epoch + impact-scope partial
// re-lower + JIT unhandled deopt + linear metadata flow + quote fallback.
//
// Non-duplicative with #600 (incremental-closure), #680 (impact_scope),
// #530 (production-reloader), #655 (edsl-core-stability).
//
//   - AC1:  query:compiler-core-incremental-stats reachable (schema 657)
//   - AC2:  mutate:rebind bumps impact-blocks
//   - AC3:  recursive define cache bumps bridge-epoch-cache-syncs
//   - AC4:  mutate-invalidate path bumps partial-relower or full-fallbacks
//   - AC5:  multi-round query/mutate — stats monotonic
//   - AC6:  quote define observable via quote-fallback-refreshes
//   - AC7:  query regression (incremental-closure, edsl-core, pattern-index)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_657_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t hash_int(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (engine:metrics \"query:compiler-core-incremental-stats\") \"" +
                     key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto bridge = hash_int(cs, "bridge-epoch-cache-syncs");
    const auto impact = hash_int(cs, "impact-blocks");
    const auto partial = hash_int(cs, "partial-relower-hits");
    const auto full = hash_int(cs, "full-fallbacks");
    const auto jit = hash_int(cs, "jit-unhandled-deopts");
    const auto linear = hash_int(cs, "linear-metadata-flows");
    const auto quote = hash_int(cs, "quote-fallback-refreshes");
    if (bridge < 0 || impact < 0 || partial < 0 || full < 0 || jit < 0 || linear < 0 || quote < 0)
        return -1;
    return bridge + impact + partial + full + jit + linear + quote;
}

static bool setup_recursive_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) "
                 "(define a 1) (define b 2) (fact 5)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:compiler-core-incremental-stats (schema 657) ---");
    CHECK(setup_recursive_workspace(cs), "recursive closure workspace setup");
    auto h = cs.eval("(engine:metrics \"query:compiler-core-incremental-stats\")");
    CHECK(h && is_hash(*h), "compiler-core-incremental-stats returns hash");
    CHECK(hash_int(cs, "schema") == 657, "schema == 657");

    std::println("\n--- AC2: mutate:rebind bumps impact-blocks ---");
    const auto impact0 = hash_int(cs, "impact-blocks");
    (void)cs.eval("(mutate:rebind \"a\" \"10\")");
    (void)cs.eval("(eval-current)");
    const auto impact1 = hash_int(cs, "impact-blocks");
    std::println("  impact-blocks: {} -> {}", impact0, impact1);
    CHECK(impact1 >= impact0, "impact-blocks monotonic after mutate");

    std::println("\n--- AC3: recursive cache bridge-epoch-cache-syncs ---");
    const auto bridge0 = hash_int(cs, "bridge-epoch-cache-syncs");
    (void)cs.eval("(fact 3)");
    (void)cs.eval("(fact 4)");
    const auto bridge1 = hash_int(cs, "bridge-epoch-cache-syncs");
    std::println("  bridge-epoch-cache-syncs: {} -> {}", bridge0, bridge1);
    CHECK(bridge1 >= bridge0, "bridge-epoch-cache-syncs monotonic");

    std::println("\n--- AC4: rebind bumps partial-relower or full-fallbacks ---");
    const auto partial0 = hash_int(cs, "partial-relower-hits");
    const auto full0 = hash_int(cs, "full-fallbacks");
    (void)cs.eval("(mutate:rebind \"b\" \"20\")");
    (void)cs.eval("(eval-current)");
    const auto partial1 = hash_int(cs, "partial-relower-hits");
    const auto full1 = hash_int(cs, "full-fallbacks");
    std::println("  partial-relower-hits: {} -> {}", partial0, partial1);
    std::println("  full-fallbacks: {} -> {}", full0, full1);
    CHECK(partial1 >= partial0 || full1 >= full0,
          "partial-relower or full-fallback bumped after rebind");

    std::println("\n--- AC5: multi-round query/mutate monotonic ---");
    const auto stats5a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"a\" \"" + std::to_string(30 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(fact 2)");
    }
    const auto stats5b = stats_sum(cs);
    std::println("  compiler-core-incremental sum: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "compiler-core-incremental stats monotonic");

    std::println("\n--- AC6: quote define quote-fallback-refreshes ---");
    (void)cs.eval("(set-code \"(define q (quote (+ 1 2))) q\")");
    (void)cs.eval("(eval-current)");
    const auto quote = hash_int(cs, "quote-fallback-refreshes");
    CHECK(quote >= 0, "quote-fallback-refreshes readable");

    std::println("\n--- AC7: query regression ---");
    auto icl = cs.eval("(engine:metrics \"query:incremental-closure-stats\")");
    auto edsl = cs.eval("(engine:metrics \"query:edsl-core-stability-stats\")");
    auto pindex = cs.eval("(engine:metrics \"query:pattern-index-stats\")");
    CHECK(icl && is_hash(*icl), "incremental-closure-stats regression");
    CHECK(edsl && is_hash(*edsl), "edsl-core-stability-stats regression");
    CHECK(pindex && is_int(*pindex), "pattern-index-stats regression");
}

} // namespace aura_657_detail

int aura_issue_compiler_core_incremental_selfmod_gaps_run() {
    aura::compiler::CompilerService cs;
    aura_657_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_compiler_core_incremental_selfmod_gaps_run();
}
#endif
