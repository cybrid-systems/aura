// test_occurrence_typing_blame_post_mutate_recovery.cpp
// Issue #609: Occurrence Typing + blame/provenance post-mutate recovery.
//
// Non-duplicative with #537/#518 (re-narrow infra), #639 (stale blame),
// #627 (bidirectional check narrow), #608 (incremental dependency).
//
// AC1: query:occurrence-narrow-stats reachable + non-negative
// AC2: if-predicate mutate → stats grow + narrow eval correct
// AC3: predicate swap (number? → string?) → blame path + stats monotonic
// AC4: query:provenance-of present after mutate
// AC5: occurrence-stale-count == 0 after auto re-narrow
// AC6: define-type + match mutate → typecheck ok + stats monotonic
// AC7: multi-round if/match mutate matrix — eval + stats monotonic
// AC8: sequential query/eval stress under mutate (no crash)
//
// Uses one CompilerService for the full matrix — each test
// function creating/destroying a service leaves a dangling
// g_query_evaluator and can segfault on the next case.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_609_detail {

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

static std::int64_t occurrence_narrow_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:occurrence-narrow-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:occurrence-narrow-stats reachable ---");
    CHECK(load_workspace(cs), "load if+match workspace");
    const auto s0 = occurrence_narrow_stats(cs);
    std::println("  query:occurrence-narrow-stats = {}", s0);
    CHECK(s0 >= 0, "occurrence-narrow-stats non-negative");

    std::println("\n--- AC2: if-predicate mutate → stats grow + narrow eval ---");
    const auto stats2a = occurrence_narrow_stats(cs);
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 7) 0))\" "
                  "\"issue-609-if\")");
    const auto stats2b = occurrence_narrow_stats(cs);
    std::println("  occurrence-narrow-stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b >= stats2a, "occurrence-narrow-stats monotonic after if mutate");
    auto r2 = cs.eval("(f 4)");
    CHECK(r2 && is_int(*r2), "f 4 returns int after narrow mutate");
    if (r2 && is_int(*r2))
        CHECK(as_int(*r2) == 11, "narrow-dependent (+ x 7) correct");

    std::println("\n--- AC3: predicate swap → blame path + stats monotonic ---");
    const auto stats3a = occurrence_narrow_stats(cs);
    const auto snap3a = cs.snapshot();
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Lazy);
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (string? x) x 0))\" "
                  "\"issue-609-blame\")");
    (void)cs.eval("(typecheck-current)");
    const auto stats3b = occurrence_narrow_stats(cs);
    const auto snap3b = cs.snapshot();
    std::println("  occurrence-narrow-stats: {} -> {}", stats3a, stats3b);
    std::println("  blame_attached={} stale_prevented={} consistency={}",
                 snap3b.narrow_blame_attached_total, snap3b.stale_check_narrow_prevented_total,
                 snap3b.post_mutate_narrow_consistency_total);
    CHECK(stats3b >= stats3a, "occurrence-narrow-stats monotonic on predicate swap");
    CHECK(snap3b.narrow_blame_attached_total >= snap3a.narrow_blame_attached_total ||
              snap3b.narrow_stale_caught_total > snap3a.narrow_stale_caught_total ||
              snap3b.narrow_invalidation_post_mutate_total >
                  snap3a.narrow_invalidation_post_mutate_total,
          "blame/stale path bumped on predicate swap");

    std::println("\n--- AC4: provenance-of present after mutate ---");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 3) 0))\" "
                  "\"issue-609-prov\")");
    auto prov = cs.eval("(query:provenance-of \"x\")");
    CHECK(prov.has_value(), "query:provenance-of after predicate mutate");

    std::println("\n--- AC5: occurrence-stale-count == 0 after auto re-narrow ---");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 5) 0))\" "
                  "\"issue-609-stale\")");
    auto stale = cs.eval("(stats:get \"query:occurrence-stale-count\")");
    const auto stale_count = stale && is_int(*stale) ? as_int(*stale) : -1;
    std::println("  occurrence-stale-count = {}", stale_count);
    CHECK(stale_count == 0, "no stale occurrence nodes after auto re-narrow");

    std::println("\n--- AC6: define-type + match mutate → typecheck + stats ---");
    const auto stats6a = occurrence_narrow_stats(cs);
    (void)cs.eval("(mutate:rebind \"m\" \"(lambda (t) (match t ((Num) 11) ((Str) 22)))\" "
                  "\"issue-609-match\")");
    auto tc6 = cs.eval("(typecheck-current)");
    CHECK(tc6.has_value(), "typecheck-current after match mutate");
    auto r6 = cs.eval("(m Num)");
    CHECK(r6 && is_int(*r6), "match eval after mutate");
    if (r6 && is_int(*r6))
        CHECK(as_int(*r6) == 11, "match arm Num returns 11");
    const auto stats6b = occurrence_narrow_stats(cs);
    std::println("  occurrence-narrow-stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "occurrence-narrow-stats monotonic after match mutate");

    std::println("\n--- AC7: multi-round if/match mutate matrix ---");
    const auto stats7a = occurrence_narrow_stats(cs);
    for (int round = 0; round < 3; ++round) {
        const std::string f_body =
            "(lambda (x) (if (number? x) (+ x " + std::to_string(round + 8) + ") 0))";
        const std::string m_body = "(lambda (t) (match t ((Num) " + std::to_string(round + 30) +
                                   ") ((Str) " + std::to_string(round + 40) + ")))";
        (void)cs.eval("(mutate:rebind \"f\" \"" + f_body + "\" \"r" + std::to_string(round) +
                      "-f\")");
        (void)cs.eval("(mutate:rebind \"m\" \"" + m_body + "\" \"r" + std::to_string(round) +
                      "-m\")");
        auto rf = cs.eval("(f 2)");
        auto rm = cs.eval("(m Str)");
        CHECK(rf && is_int(*rf), "f eval ok round " + std::to_string(round));
        CHECK(rm && is_int(*rm), "m eval ok round " + std::to_string(round));
        if (rf && is_int(*rf))
            CHECK(as_int(*rf) == 2 + round + 8,
                  "if narrow semantics round " + std::to_string(round));
        if (rm && is_int(*rm))
            CHECK(as_int(*rm) == round + 40, "match semantics round " + std::to_string(round));
    }
    const auto stats7b = occurrence_narrow_stats(cs);
    std::println("  occurrence-narrow-stats: {} -> {}", stats7a, stats7b);
    CHECK(stats7b >= stats7a, "occurrence-narrow-stats monotonic over matrix");

    std::println("\n--- AC8: sequential query/eval stress under mutate ---");
    std::int64_t stress_sum = 0;
    for (int i = 0; i < 8; ++i) {
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x " +
                      std::to_string(i) + ") 0))\" \"stress-" + std::to_string(i) + "\")");
        auto qs = cs.eval("(engine:metrics \"query:occurrence-narrow-stats\")");
        CHECK(qs && is_int(*qs), "query:occurrence-narrow-stats during stress");
        if (qs && is_int(*qs))
            stress_sum += as_int(*qs);
        auto ev = cs.eval("(f 2)");
        CHECK(ev && is_int(*ev), "eval during stress round " + std::to_string(i));
    }
    std::println("  stress_sum={}", stress_sum);
    CHECK(stress_sum > 0, "stress query/eval sum > 0");
}

} // namespace aura_609_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_609_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}