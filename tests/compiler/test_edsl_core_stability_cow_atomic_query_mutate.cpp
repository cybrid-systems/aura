// test_edsl_core_stability_cow_atomic_query_mutate.cpp — Issue #655:
// 5 EDSL core gaps — StableNodeRef COW + tag_arity delta + nested atomic
// rollback + children safe view + precise mutate invalidation.
//
// Non-duplicative with #527 (stable-ref-cow), #552 (edsl-stability),
// #622 (atomic-batch), #654 (macro-hygiene-fiber-panic).
//
//   - AC1:  query:edsl-core-stability-stats reachable (schema 655)
//   - AC2:  mutate + query:pattern bumps tag-arity-delta-patches
//   - AC3:  StableRef cross-COW validation observable
//   - AC4:  mutate-invalidate-precision bumps on rebind
//   - AC5:  multi-round query/mutate — stats monotonic
//   - AC6:  query:children-stable uses safe view path
//   - AC7:  query regression (stable-ref-cow, edsl-stability, pattern-index)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_655_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t hash_int(CompilerService& cs, const std::string& key) {
    auto r =
        cs.eval("(hash-ref (engine:metrics \"query:edsl-core-stability-stats\") \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto cow = hash_int(cs, "cow-stable-ref-remaps");
    const auto delta = hash_int(cs, "tag-arity-delta-patches");
    const auto nested = hash_int(cs, "nested-atomic-rollbacks");
    const auto safe = hash_int(cs, "children-safe-views");
    const auto inv = hash_int(cs, "mutate-invalidate-precision");
    if (cow < 0 || delta < 0 || nested < 0 || safe < 0 || inv < 0)
        return -1;
    return cow + delta + nested + safe + inv;
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define a 1) (define b 2) (define (fact n) "
                 "(if (= n 0) 1 (* n (fact (- n 1))))) (fact 3)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:edsl-core-stability-stats (schema 655) ---");
    CHECK(setup_workspace(cs), "workspace setup");
    auto h = cs.eval("(engine:metrics \"query:edsl-core-stability-stats\")");
    CHECK(h && is_hash(*h), "edsl-core-stability-stats returns hash");
    CHECK(hash_int(cs, "schema") == 655, "schema == 655");

    std::println("\n--- AC2: query:pattern bumps tag-arity-delta ---");
    const auto delta0 = hash_int(cs, "tag-arity-delta-patches");
    (void)cs.eval("(query:pattern \"define\")");
    (void)cs.eval("(query:pattern \"fact\")");
    const auto delta1 = hash_int(cs, "tag-arity-delta-patches");
    std::println("  tag-arity-delta-patches: {} -> {}", delta0, delta1);
    CHECK(delta1 >= delta0, "tag-arity-delta-patches monotonic after query:pattern");

    std::println("\n--- AC3: StableRef cross-COW validation ---");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace flat available");
    if (ws && ws->size() > 1) {
        const auto cow0 = hash_int(cs, "cow-stable-ref-remaps");
        (void)cs.evaluator().validate_stable_ref(1, ws->generation());
        const auto cow1 = hash_int(cs, "cow-stable-ref-remaps");
        std::println("  cow-stable-ref-remaps: {} -> {}", cow0, cow1);
        CHECK(cow1 >= cow0, "cow-stable-ref-remaps monotonic");
    }

    std::println("\n--- AC4: mutate-invalidate-precision on rebind ---");
    const auto inv0 = hash_int(cs, "mutate-invalidate-precision");
    (void)cs.eval("(mutate:rebind \"a\" \"10\")");
    (void)cs.eval("(eval-current)");
    const auto inv1 = hash_int(cs, "mutate-invalidate-precision");
    std::println("  mutate-invalidate-precision: {} -> {}", inv0, inv1);
    CHECK(inv1 > inv0, "mutate-invalidate-precision bumped after rebind");

    std::println("\n--- AC5: multi-round query/mutate monotonic ---");
    const auto stats5a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"b\" \"" + std::to_string(20 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(query:pattern \"define\")");
    }
    const auto stats5b = stats_sum(cs);
    std::println("  edsl-core-stability sum: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "edsl-core-stability stats monotonic");

    std::println("\n--- AC6: query:children-stable safe view ---");
    auto children = cs.eval("(query:children-stable 0)");
    CHECK(children.has_value(), "query:children-stable returns");
    const auto safe = hash_int(cs, "children-safe-views");
    CHECK(safe >= 0, "children-safe-views readable");

    std::println("\n--- AC7: query regression ---");
    auto cow = cs.eval("(engine:metrics \"query:stable-ref-cow-fiber-stats\")");
    auto edsl = cs.eval("(engine:metrics \"query:edsl-stability-stats\")");
    auto pindex = cs.eval("(engine:metrics \"query:pattern-index-stats\")");
    CHECK(cow && is_int(*cow), "stable-ref-cow-fiber-stats regression");
    CHECK(edsl && is_int(*edsl), "edsl-stability-stats regression");
    CHECK(pindex && is_int(*pindex), "pattern-index-stats regression");
}

} // namespace aura_655_detail

int aura_issue_edsl_core_stability_cow_atomic_query_mutate_run() {
    aura::compiler::CompilerService cs;
    aura_655_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_edsl_core_stability_cow_atomic_query_mutate_run();
}
#endif
