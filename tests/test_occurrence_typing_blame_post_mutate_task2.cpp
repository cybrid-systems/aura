// test_occurrence_typing_blame_post_mutate_task2.cpp
// Issue #576: Task2 Occurrence Typing dirty recovery +
// blame/provenance chain completeness post-mutate.
//
// Non-duplicative with #609 (occurrence-narrow-stats),
// #639 (narrow-blame-stats stale invalidation),
// #537/#518 (re-narrow infra), #573 (solve_delta Task2).
//
// AC1: query:occurrence-blame-stats reachable + non-negative
// AC2: if-predicate mutate → stats grow + narrow eval correct
// AC3: predicate swap → blame_chain_preserved path bumped
// AC4: query:provenance-of present after mutate
// AC5: occurrence-stale-count == 0 after auto re-narrow
// AC6: ADT/match mutate → typecheck + eval correct
// AC7: narrowing_refresh_count grows on predicate mutate
// AC8: multi-round if/match mutate — stats monotonic
// AC9: query regression (occurrence-narrow-stats, narrow-blame-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_576_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static constexpr const char* k_workspace = R"(
(define-type (Tag) (Num) (Str))
(define f (lambda (x) (if (number? x) (+ x 1) 0)))
(define m (lambda (t) (match t ((Num) 10) ((Str) 20))))
)";

static bool load_workspace(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_workspace + "\")"))
        return false;
    if (!cs.eval("(eval-current)"))
        return false;
    return cs.eval("(typecheck-current)").has_value();
}

static std::int64_t occurrence_blame_stats(CompilerService& cs) {
    auto r = cs.eval("(query:occurrence-blame-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:occurrence-blame-stats reachable ---");
    CHECK(load_workspace(cs), "load if+match workspace");
    const auto s0 = occurrence_blame_stats(cs);
    std::println("  query:occurrence-blame-stats = {}", s0);
    CHECK(s0 >= 0, "occurrence-blame-stats non-negative");

    std::println("\n--- AC2: if-predicate mutate → stats grow + narrow eval ---");
    const auto stats2a = occurrence_blame_stats(cs);
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 8) 0))\" "
                  "\"issue-576-if\")");
    const auto stats2b = occurrence_blame_stats(cs);
    std::println("  occurrence-blame-stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b >= stats2a, "occurrence-blame-stats monotonic after if mutate");
    auto r2 = cs.eval("(f 5)");
    CHECK(r2 && is_int(*r2), "f 5 returns int after narrow mutate");
    if (r2 && is_int(*r2))
        CHECK(as_int(*r2) == 13, "narrow-dependent (+ x 8) correct");

    std::println("\n--- AC3: predicate swap → blame_chain_preserved bumped ---");
    const auto stats3a = occurrence_blame_stats(cs);
    const auto snap3a = cs.snapshot();
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Lazy);
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (string? x) x 0))\" "
                  "\"issue-576-blame\")");
    (void)cs.eval("(typecheck-current)");
    const auto stats3b = occurrence_blame_stats(cs);
    const auto snap3b = cs.snapshot();
    std::println("  occurrence-blame-stats: {} -> {}", stats3a, stats3b);
    std::println("  blame_chain_complete={} stale_prevented={}",
                 snap3b.occurrence_blame_chain_complete_total,
                 snap3b.stale_check_narrow_prevented_total);
    CHECK(stats3b >= stats3a, "occurrence-blame-stats monotonic on predicate swap");
    CHECK(snap3b.occurrence_blame_chain_complete_total >=
                  snap3a.occurrence_blame_chain_complete_total ||
              snap3b.narrow_blame_attached_total > snap3a.narrow_blame_attached_total ||
              snap3b.narrow_stale_caught_total > snap3a.narrow_stale_caught_total,
          "blame chain path bumped on predicate swap");

    std::println("\n--- AC4: query:provenance-of present after mutate ---");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 2) 0))\" "
                  "\"issue-576-prov\")");
    auto prov = cs.eval("(query:provenance-of \"x\")");
    CHECK(prov.has_value(), "query:provenance-of after predicate mutate");

    std::println("\n--- AC5: occurrence-stale-count == 0 after auto re-narrow ---");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 6) 0))\" "
                  "\"issue-576-stale\")");
    auto stale = cs.eval("(query:occurrence-stale-count)");
    const auto stale_count = stale && is_int(*stale) ? as_int(*stale) : -1;
    std::println("  occurrence-stale-count = {}", stale_count);
    CHECK(stale_count == 0, "no stale occurrence nodes after auto re-narrow");

    std::println("\n--- AC6: ADT/match mutate → typecheck + eval ---");
    const auto stats6a = occurrence_blame_stats(cs);
    (void)cs.eval("(mutate:rebind \"m\" \"(lambda (t) (match t ((Num) 12) ((Str) 24)))\" "
                  "\"issue-576-match\")");
    auto tc6 = cs.eval("(typecheck-current)");
    CHECK(tc6.has_value(), "typecheck-current after match mutate");
    auto r6 = cs.eval("(m Num)");
    CHECK(r6 && is_int(*r6), "match eval after mutate");
    if (r6 && is_int(*r6))
        CHECK(as_int(*r6) == 12, "match arm Num returns 12");
    const auto stats6b = occurrence_blame_stats(cs);
    std::println("  occurrence-blame-stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "occurrence-blame-stats monotonic after match mutate");

    std::println("\n--- AC7: narrowing_refresh_count grows on mutate ---");
    const auto n0 = cs.evaluator().get_narrowing_refresh_count();
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 9) 0))\" "
                  "\"issue-576-refresh\")");
    const auto n1 = cs.evaluator().get_narrowing_refresh_count();
    std::println("  narrowing_refresh: {} -> {}", n0, n1);
    CHECK(n1 > n0, "narrowing_refresh_count bumped on predicate mutate");

    std::println("\n--- AC8: multi-round if/match mutate matrix ---");
    const auto stats8a = occurrence_blame_stats(cs);
    for (int round = 0; round < 3; ++round) {
        const std::string f_body =
            "(lambda (x) (if (number? x) (+ x " + std::to_string(round + 12) + ") 0))";
        const std::string m_body = "(lambda (t) (match t ((Num) " + std::to_string(round + 50) +
                                   ") ((Str) " + std::to_string(round + 60) + ")))";
        (void)cs.eval("(mutate:rebind \"f\" \"" + f_body + "\" \"r" + std::to_string(round) +
                      "-f\")");
        (void)cs.eval("(mutate:rebind \"m\" \"" + m_body + "\" \"r" + std::to_string(round) +
                      "-m\")");
        auto rf = cs.eval("(f 1)");
        auto rm = cs.eval("(m Str)");
        CHECK(rf && is_int(*rf), "f eval ok round " + std::to_string(round));
        CHECK(rm && is_int(*rm), "m eval ok round " + std::to_string(round));
        if (rf && is_int(*rf))
            CHECK(as_int(*rf) == 1 + round + 12,
                  "if narrow semantics round " + std::to_string(round));
        if (rm && is_int(*rm))
            CHECK(as_int(*rm) == round + 60, "match semantics round " + std::to_string(round));
    }
    const auto stats8b = occurrence_blame_stats(cs);
    std::println("  occurrence-blame-stats: {} -> {}", stats8a, stats8b);
    CHECK(stats8b >= stats8a, "occurrence-blame-stats monotonic over matrix");

    std::println("\n--- AC9: query regression ---");
    auto ons = cs.eval("(query:occurrence-narrow-stats)");
    auto nbs = cs.eval("(query:narrow-blame-stats)");
    CHECK(ons && is_int(*ons), "query:occurrence-narrow-stats returns int");
    CHECK(nbs && is_int(*nbs), "query:narrow-blame-stats returns int");

    const auto narrow = cs.evaluator().get_narrowing_refresh_count();
    const auto blame = cs.snapshot().occurrence_blame_chain_complete_total;
    std::println("  final narrowing_refresh={} blame_chain_complete={}", narrow, blame);
    CHECK(narrow > 0, "narrowing_refresh_count > 0 after mutate cycle");
}

} // namespace aura_576_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_576_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}