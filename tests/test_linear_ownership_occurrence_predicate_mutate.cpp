// test_linear_ownership_occurrence_predicate_mutate.cpp — Issue #747:
// OwnershipEnv post-mutate revalidate + Occurrence Typing predicate
// branches for linear safety under typed mutation.
//
// Non-duplicative with #688, #689, #740, #746, #659.
//
//   - AC1: query:linear-occurrence-mutate-stats reachable (schema 747)
//   - AC2: linear + predicate mutate bumps revalidate / branch-safe stats
//   - AC3: eval correctness after predicate+linear mutate cycle
//   - AC4: multi-round mutate matrix monotonic
//   - AC5: query regression (linear-ownership, occurrence, jit-typed-mutation)

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_747_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t loc_hash(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (engine:metrics \"query:linear-occurrence-mutate-stats\") \"" +
                     key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto reval = loc_hash(cs, "revalidate-hits");
    const auto escape = loc_hash(cs, "escape-violations-prevented");
    const auto safe = loc_hash(cs, "predicate-branch-linear-safe");
    if (reval < 0 || escape < 0 || safe < 0)
        return -1;
    return reval + escape + safe;
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define f (lambda (x) "
                 "(if (number? x) "
                 "(let ((l (Linear x))) (move l)) "
                 "0))) "
                 "(f 5)\")")) {
        return false;
    }
    if (!cs.eval("(eval-current)").has_value())
        return false;
    return cs.eval("(typecheck-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:linear-occurrence-mutate-stats (schema 747) ---");
    CHECK(setup_workspace(cs), "linear+occurrence predicate workspace setup");
    auto h = cs.eval("(engine:metrics \"query:linear-occurrence-mutate-stats\")");
    CHECK(h && is_hash(*h), "linear-occurrence-mutate-stats returns hash");
    CHECK(loc_hash(cs, "schema") == 747, "schema == 747");
    CHECK(loc_hash(cs, "revalidate-hits") >= 0, "revalidate-hits present");
    CHECK(loc_hash(cs, "escape-violations-prevented") >= 0, "escape-violations-prevented present");
    CHECK(loc_hash(cs, "predicate-branch-linear-safe") >= 0,
          "predicate-branch-linear-safe present");

    std::println("\n--- AC2: mutate on predicate+linear bumps stats ---");
    const auto reval0 = loc_hash(cs, "revalidate-hits");
    const auto safe0 = loc_hash(cs, "predicate-branch-linear-safe");
    const auto stats2a = stats_sum(cs);
    (void)cs.eval("(f 3)");
    auto reb = cs.eval("(mutate:rebind \"f\" "
                       "\"(lambda (x) (if (number? x) "
                       "(let ((l (Linear (+ x 1)))) (move l)) 0))\" "
                       "\"issue-747\")");
    CHECK(reb && aura::compiler::types::is_bool(*reb) && aura::compiler::types::as_bool(*reb),
          "mutate:rebind on linear predicate f succeeds");
    CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
    (void)cs.eval("(typecheck-current)");
    (void)cs.eval("(eval-current :jit)");
    const auto reval1 = loc_hash(cs, "revalidate-hits");
    const auto safe1 = loc_hash(cs, "predicate-branch-linear-safe");
    const auto stats2b = stats_sum(cs);
    std::println("  revalidate-hits: {} -> {}", reval0, reval1);
    std::println("  predicate-branch-linear-safe: {} -> {}", safe0, safe1);
    std::println("  linear-occurrence sum: {} -> {}", stats2a, stats2b);
    CHECK(stats2b >= stats2a, "linear-occurrence stats monotonic after mutate");
    CHECK(safe1 > safe0 || reval1 > reval0,
          "revalidate or predicate-branch-linear-safe grew after mutate");

    std::println("\n--- AC3: eval correctness post mutate ---");
    auto v = cs.eval("(f 4)");
    CHECK(v && is_int(*v) && as_int(*v) == 5, "(f 4) == 5 after predicate+linear mutate");
    auto nz = cs.eval("(f \"hi\")");
    CHECK(nz && is_int(*nz) && as_int(*nz) == 0, "(f \"hi\") == 0 on non-number branch");

    std::println("\n--- AC4: multi-round predicate mutate matrix ---");
    const auto stats4a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        const std::string body = "(lambda (x) (if (number? x) "
                                 "(let ((l (Linear (+ x " +
                                 std::to_string(round) + ")))) (move l)) 0))";
        std::string esc;
        esc.reserve(body.size() + 8);
        for (char c : body) {
            if (c == '\\' || c == '"')
                esc.push_back('\\');
            esc.push_back(c);
        }
        (void)cs.eval(std::format("(mutate:rebind \"f\" \"{}\" \"r{}-747\")", esc, round));
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(f 2)");
    }
    const auto stats4b = stats_sum(cs);
    std::println("  linear-occurrence sum: {} -> {}", stats4a, stats4b);
    CHECK(stats4b >= stats4a, "stats monotonic over mutate matrix");

    std::println("\n--- AC5: query regression ---");
    auto lot = cs.eval("(engine:metrics \"query:linear-ownership-typed-mutate-stats\")");
    auto otm = cs.eval("(engine:metrics \"query:occurrence-typing-mutate-stats\")");
    auto jtm = cs.eval("(engine:metrics \"query:jit-typed-mutation-stats\")");
    CHECK(lot && is_hash(*lot), "linear-ownership-typed-mutate-stats regression");
    CHECK(otm && is_hash(*otm), "occurrence-typing-mutate-stats regression");
    CHECK(jtm && is_hash(*jtm), "jit-typed-mutation-stats regression");
}

} // namespace aura_issue_747_detail

int aura_issue_linear_ownership_occurrence_predicate_mutate_run() {
    aura::compiler::CompilerService cs;
    aura_issue_747_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_linear_ownership_occurrence_predicate_mutate_run();
}
#endif
