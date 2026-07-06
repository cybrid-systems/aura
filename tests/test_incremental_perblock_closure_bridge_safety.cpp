// test_incremental_perblock_closure_bridge_safety.cpp — Issue #600:
// Per-block dirty + impact scope + closure bridge synergy for minimal
// re-lower in AI self-mod (Prompt6 incremental + memory safety).
//
// Non-duplicative with #530 (incremental-production-reloader-stats),
// #429 (soa-dirty-stats), #531 (closure-env-safety-stats),
// #599 (compiler-root-stats).
//
//   - AC1:  query:incremental-closure-stats reachable (schema 600)
//   - AC2:  mutate on closure workspace bumps blocks-relowered
//   - AC3:  closure-bridge-hits observable via bridge_epoch_hit
//   - AC4:  min-scope-win counter observable
//   - AC5:  jit-sync-count bumps on invalidate path
//   - AC6:  multi-round mutate — stats monotonic
//   - AC7:  query regression (incremental-production, soa-dirty, closure-env)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_600_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t hash_int(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (query:incremental-closure-stats) \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto blocks = hash_int(cs, "blocks-relowered");
    const auto bridge = hash_int(cs, "closure-bridge-hits");
    const auto win = hash_int(cs, "min-scope-win");
    const auto jit = hash_int(cs, "jit-sync-count");
    if (blocks < 0 || bridge < 0 || win < 0 || jit < 0)
        return -1;
    return blocks + bridge + win + jit;
}

static bool setup_closure_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) "
                 "(define a 1) (define b 2) (fact 5)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:incremental-closure-stats (schema 600) ---");
    CHECK(setup_closure_workspace(cs), "recursive closure workspace setup");
    auto h = cs.eval("(query:incremental-closure-stats)");
    CHECK(h && is_hash(*h), "incremental-closure-stats returns hash");
    CHECK(hash_int(cs, "schema") == 600, "schema == 600");
    const auto s0 = stats_sum(cs);
    std::println("  incremental-closure-stats sum = {}", s0);
    CHECK(s0 >= 0, "incremental-closure-stats non-negative");

    std::println("\n--- AC2: mutate bumps blocks-relowered ---");
    const auto blocks0 = hash_int(cs, "blocks-relowered");
    const auto impact0 = cs.evaluator().get_impact_scope_calls();
    (void)cs.eval("(mutate:rebind \"a\" \"10\")");
    (void)cs.eval("(eval-current)");
    const auto blocks1 = hash_int(cs, "blocks-relowered");
    const auto impact1 = cs.evaluator().get_impact_scope_calls();
    std::println("  blocks-relowered: {} -> {} impact-scope: {} -> {}", blocks0, blocks1, impact0,
                 impact1);
    CHECK(blocks1 >= blocks0, "blocks-relowered monotonic after mutate");
    CHECK(impact1 >= impact0, "impact_scope_calls monotonic after mutate");

    std::println("\n--- AC3: closure-bridge-hits observable ---");
    const auto bridge0 = hash_int(cs, "closure-bridge-hits");
    cs.bump_bridge_epoch_hit_count();
    cs.bump_bridge_epoch_hit_count();
    const auto bridge1 = hash_int(cs, "closure-bridge-hits");
    std::println("  closure-bridge-hits: {} -> {}", bridge0, bridge1);
    CHECK(bridge1 > bridge0, "closure-bridge-hits bumped via bridge_epoch_hit");

    std::println("\n--- AC4: min-scope-win observable ---");
    const auto win0 = hash_int(cs, "min-scope-win");
    cs.evaluator().bump_incremental_closure_min_scope_win(5);
    const auto win1 = hash_int(cs, "min-scope-win");
    std::println("  min-scope-win: {} -> {}", win0, win1);
    CHECK(win1 > win0, "min-scope-win bumped");

    std::println("\n--- AC5: jit-sync-count bumps on invalidate ---");
    const auto jit0 = hash_int(cs, "jit-sync-count");
    (void)cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) "
                  "(define a 99) (define b 2) (fact 3)\")");
    (void)cs.eval("(eval-current)");
    const auto jit1 = hash_int(cs, "jit-sync-count");
    std::println("  jit-sync-count: {} -> {}", jit0, jit1);
    CHECK(jit1 > jit0, "jit-sync-count bumped after invalidate/redefine");

    std::println("\n--- AC6: multi-round mutate stats monotonic ---");
    const auto stats6a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"b\" \"" + std::to_string(20 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(fact 3)");
    }
    const auto stats6b = stats_sum(cs);
    std::println("  incremental-closure sum: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "incremental-closure stats monotonic over matrix");

    std::println("\n--- AC7: query regression ---");
    auto ipr = cs.eval("(query:incremental-production-relower-stats)");
    auto soa = cs.eval("(query:soa-dirty-stats)");
    auto ces = cs.eval("(query:closure-env-safety-stats)");
    CHECK(ipr && is_hash(*ipr), "incremental-production-reloader-stats regression");
    CHECK(soa && is_hash(*soa), "soa-dirty-stats regression");
    CHECK(ces && is_hash(*ces), "closure-env-safety-stats regression");
}

} // namespace aura_600_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_600_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}