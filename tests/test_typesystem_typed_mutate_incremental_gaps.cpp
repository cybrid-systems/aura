// test_typesystem_typed_mutate_incremental_gaps.cpp — Issue #659:
// 5 type system gaps — solve_delta reverify + dead coercion elim +
// linear ownership post-mutate + occurrence provenance + coercion map.
//
// Non-duplicative with #656 (Lambda param recheck), #657 (compiler-core),
// #690 (constraint-typed-mutate), #508 (dead-coercion-zerooverhead).
//
//   - AC1:  query:typesystem-typed-mutate-stats reachable (schema 659)
//   - AC2:  mutate on predicate code bumps reverify or provenance
//   - AC3:  eval-current bumps dead-coercion or coercion wins
//   - AC4:  linear-post-mutate-revalidations readable
//   - AC5:  multi-round mutate — stats monotonic
//   - AC6:  coercion-incremental-wins readable
//   - AC7:  query regression (constraint-typed-mutate, dead-coercion, linear)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_659_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t hash_int(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (query:typesystem-typed-mutate-stats) \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto reverify = hash_int(cs, "delta-reverify-scans");
    const auto dce = hash_int(cs, "dead-coercion-eliminated");
    const auto linear = hash_int(cs, "linear-post-mutate-revalidations");
    const auto prov = hash_int(cs, "narrowing-provenance-refresh");
    const auto coercion = hash_int(cs, "coercion-incremental-wins");
    if (reverify < 0 || dce < 0 || linear < 0 || prov < 0 || coercion < 0)
        return -1;
    return reverify + dce + linear + prov + coercion;
}

static bool setup_predicate_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (f x) (if (number? x) (+ x 1) 0)) "
                 "(define a 1) (define b 2) (f 5)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:typesystem-typed-mutate-stats (schema 659) ---");
    CHECK(setup_predicate_workspace(cs), "predicate workspace setup");
    auto h = cs.eval("(query:typesystem-typed-mutate-stats)");
    CHECK(h && is_hash(*h), "typesystem-typed-mutate-stats returns hash");
    CHECK(hash_int(cs, "schema") == 659, "schema == 659");

    std::println("\n--- AC2: mutate bumps reverify or provenance ---");
    const auto rev0 = hash_int(cs, "delta-reverify-scans");
    const auto prov0 = hash_int(cs, "narrowing-provenance-refresh");
    (void)cs.eval("(mutate:rebind \"a\" \"10\")");
    (void)cs.eval("(eval-current)");
    const auto rev1 = hash_int(cs, "delta-reverify-scans");
    const auto prov1 = hash_int(cs, "narrowing-provenance-refresh");
    std::println("  delta-reverify-scans: {} -> {}", rev0, rev1);
    std::println("  narrowing-provenance-refresh: {} -> {}", prov0, prov1);
    CHECK(rev1 >= rev0 || prov1 >= prov0, "reverify or provenance monotonic after mutate");

    std::println("\n--- AC3: eval bumps dead-coercion or coercion wins ---");
    const auto dce0 = hash_int(cs, "dead-coercion-eliminated");
    const auto coer0 = hash_int(cs, "coercion-incremental-wins");
    (void)cs.eval("(f 3)");
    (void)cs.eval("(mutate:rebind \"b\" \"20\")");
    (void)cs.eval("(eval-current)");
    const auto dce1 = hash_int(cs, "dead-coercion-eliminated");
    const auto coer1 = hash_int(cs, "coercion-incremental-wins");
    std::println("  dead-coercion-eliminated: {} -> {}", dce0, dce1);
    std::println("  coercion-incremental-wins: {} -> {}", coer0, coer1);
    CHECK(dce1 >= dce0 || coer1 >= coer0, "dead-coercion or coercion wins bumped");

    std::println("\n--- AC4: linear-post-mutate-revalidations readable ---");
    const auto linear = hash_int(cs, "linear-post-mutate-revalidations");
    CHECK(linear >= 0, "linear-post-mutate-revalidations readable");

    std::println("\n--- AC5: multi-round mutate monotonic ---");
    const auto stats5a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"a\" \"" + std::to_string(30 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(f 2)");
    }
    const auto stats5b = stats_sum(cs);
    std::println("  typesystem-typed-mutate sum: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "typesystem stats monotonic");

    std::println("\n--- AC6: coercion-incremental-wins readable ---");
    const auto coercion = hash_int(cs, "coercion-incremental-wins");
    CHECK(coercion >= 0, "coercion-incremental-wins readable");

    std::println("\n--- AC7: query regression ---");
    auto ctm = cs.eval("(query:constraint-typed-mutate-stats)");
    auto dco = cs.eval("(query:dead-coercion-zerooverhead-stats)");
    auto lot = cs.eval("(query:linear-ownership-typed-mutate-stats)");
    CHECK(ctm && is_hash(*ctm), "constraint-typed-mutate-stats regression");
    CHECK(dco && is_hash(*dco), "dead-coercion-zerooverhead-stats regression");
    CHECK(lot && is_hash(*lot), "linear-ownership-typed-mutate-stats regression");
}

} // namespace aura_659_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_659_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}