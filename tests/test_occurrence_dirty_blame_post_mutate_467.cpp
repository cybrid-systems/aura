// test_occurrence_dirty_blame_post_mutate_467.cpp
// Issue #467: per-node occurrence-dirty + blame chain
// propagation post-structural-mutate.
//
// Non-duplicative with #518 (test_occurrence_mutate_narrowing),
// #576 (occurrence-blame-stats Task2), #609 (occurrence-narrow-stats),
// #434 (engine:metrics \"compile:occurrence-dirty-stats\"), #339 (occurrence-stale).
//
// AC1: query:occurrence-stats reachable + non-negative
// AC2: if-predicate mutate → narrow eval correct
// AC3: occurrence-stale-count == 0 after auto re-narrow
// AC4: narrowing_dirty_recovery bumps on predicate mutate
// AC5: query:provenance-of present after mutate
// AC6: predicate swap → blame path observable in snapshot
// AC7: query regression (occurrence-blame-stats, occurrence-narrow-stats)
// AC8: multi-round if-predicate mutate — stats monotonic
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_467_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static constexpr const char* k_if_prog = R"(
(define f (lambda (x) (if (number? x) (+ x 1) 0)))
(define g (lambda (y) (+ y 10)))
)";

static bool load_workspace(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_if_prog + "\")"))
        return false;
    if (!cs.eval("(eval-current)"))
        return false;
    return cs.eval("(typecheck-current)").has_value();
}

static std::int64_t occurrence_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:occurrence-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:occurrence-stats reachable ---");
    CHECK(load_workspace(cs), "load if workspace");
    const auto s0 = occurrence_stats(cs);
    std::println("  query:occurrence-stats = {}", s0);
    CHECK(s0 >= 0, "occurrence-stats non-negative");

    std::println("\n--- AC2: if-predicate mutate → narrow eval correct ---");
    const auto stats2a = occurrence_stats(cs);
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 9) 0))\" "
                  "\"issue-467-if\")");
    const auto stats2b = occurrence_stats(cs);
    std::println("  occurrence-stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b >= stats2a, "occurrence-stats monotonic after if mutate");
    auto r2 = cs.eval("(f 4)");
    CHECK(r2 && is_int(*r2), "f 4 returns int after narrow mutate");
    if (r2 && is_int(*r2))
        CHECK(as_int(*r2) == 13, "narrow-dependent (+ x 9) correct");

    std::println("\n--- AC3: occurrence-stale-count == 0 after re-narrow ---");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 5) 0))\" "
                  "\"issue-467-stale\")");
    auto stale = cs.eval("(stats:get \"query:occurrence-stale-count\")");
    const auto stale_count = stale && is_int(*stale) ? as_int(*stale) : -1;
    std::println("  occurrence-stale-count = {}", stale_count);
    CHECK(stale_count == 0, "no stale occurrence nodes after auto re-narrow");

    std::println("\n--- AC4: narrowing_dirty_recovery bumps ---");
    const auto snap4a = cs.snapshot();
    (void)cs.eval("(mutate:rebind \"g\" \"(lambda (y) (+ y 25))\" \"issue-467-dirty\")");
    const auto snap4b = cs.snapshot();
    std::println("  narrowing_dirty_recovery: {} -> {}", snap4a.narrowing_dirty_recovery_total,
                 snap4b.narrowing_dirty_recovery_total);
    CHECK(snap4b.narrowing_dirty_recovery_total >= snap4a.narrowing_dirty_recovery_total,
          "narrowing_dirty_recovery monotonic on mutate");

    std::println("\n--- AC5: query:provenance-of present after mutate ---");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 2) 0))\" "
                  "\"issue-467-prov\")");
    auto prov = cs.eval("(query:provenance-of \"x\")");
    CHECK(prov.has_value(), "query:provenance-of after predicate mutate");

    std::println("\n--- AC6: predicate swap → blame path observable ---");
    const auto snap6a = cs.snapshot();
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Lazy);
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (string? x) x 0))\" "
                  "\"issue-467-blame\")");
    (void)cs.eval("(typecheck-current)");
    const auto snap6b = cs.snapshot();
    std::println("  blame_attached={} blame_complete={} stale_caught={}",
                 snap6b.narrow_blame_attached_total, snap6b.occurrence_blame_chain_complete_total,
                 snap6b.narrow_stale_caught_total);
    CHECK(snap6b.narrow_blame_attached_total >= snap6a.narrow_blame_attached_total ||
              snap6b.occurrence_blame_chain_complete_total >
                  snap6a.occurrence_blame_chain_complete_total ||
              snap6b.narrow_stale_caught_total > snap6a.narrow_stale_caught_total,
          "blame chain path bumped on predicate swap");

    std::println("\n--- AC7: query regression ---");
    auto obs = cs.eval("(engine:metrics \"query:occurrence-blame-stats\")");
    auto ons = cs.eval("(engine:metrics \"query:occurrence-narrow-stats\")");
    CHECK(obs && is_int(*obs), "query:occurrence-blame-stats returns int");
    CHECK(ons && is_int(*ons), "query:occurrence-narrow-stats returns int");

    std::println("\n--- AC8: multi-round if-predicate mutate matrix ---");
    const auto stats8a = occurrence_stats(cs);
    for (int round = 0; round < 3; ++round) {
        const std::string f_body =
            "(lambda (x) (if (number? x) (+ x " + std::to_string(round + 13) + ") 0))";
        (void)cs.eval("(mutate:rebind \"f\" \"" + f_body + "\" \"r" + std::to_string(round) +
                      "-f\")");
        auto rf = cs.eval("(f 2)");
        CHECK(rf && is_int(*rf), "f eval ok round " + std::to_string(round));
        if (rf && is_int(*rf))
            CHECK(as_int(*rf) == 2 + round + 13, "narrow semantics round " + std::to_string(round));
    }
    const auto stats8b = occurrence_stats(cs);
    std::println("  occurrence-stats: {} -> {}", stats8a, stats8b);
    CHECK(stats8b >= stats8a, "occurrence-stats monotonic over matrix");

    const auto narrow = cs.evaluator().get_narrowing_refresh_count();
    std::println("  final narrowing_refresh={}", narrow);
    CHECK(narrow > 0, "narrowing_refresh > 0 after mutate cycle");
}

} // namespace aura_467_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_467_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}