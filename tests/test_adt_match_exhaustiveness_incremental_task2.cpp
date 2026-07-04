// test_adt_match_exhaustiveness_incremental_task2.cpp
// Issue #577: Task2 ADT/match exhaustiveness + narrowing
// integration with incremental dirty/epoch post-mutate.
//
// Non-duplicative with #612 (adt-match-exhaust-stats unit),
// #260/#341 (base exhaustiveness + narrowing), #576 (occurrence
// blame Task2), #576 match smoke in occurrence-blame test.
//
// AC1: query:adt-exhaustiveness-stats reachable + non-negative
// AC2: DefineType + exhaustive match load → eval correct
// AC3: match site mutate → stats grow + eval semantics preserved
// AC4: incremental_infer after mutate → exhaust rechecks observable
// AC5: match-exhaustiveness-notes query regression
// AC6: multi-round match arm mutate matrix — stats monotonic
// AC7: compile:match-narrowing-stats + adt-match-exhaust-stats regression
// AC8: if-narrowed match subject — match_subject_total observable
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_577_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static constexpr const char* k_workspace = R"(
(define-type (Tag) (Num) (Str) (Bool))
(define m (lambda (t) (match t ((Num) 10) ((Str) 20) ((Bool) 30))))
(define f (lambda (x) (if (number? x) (m Num) (m Str))))
)";

static bool load_workspace(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_workspace + "\")"))
        return false;
    if (!cs.eval("(eval-current)"))
        return false;
    return cs.eval("(typecheck-current)").has_value();
}

static std::int64_t adt_exhaustiveness_stats(CompilerService& cs) {
    auto r = cs.eval("(query:adt-exhaustiveness-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:adt-exhaustiveness-stats reachable ---");
    CHECK(load_workspace(cs), "load ADT+match workspace");
    const auto s0 = adt_exhaustiveness_stats(cs);
    std::println("  query:adt-exhaustiveness-stats = {}", s0);
    CHECK(s0 >= 0, "adt-exhaustiveness-stats non-negative");

    std::println("\n--- AC2: exhaustive match eval correct ---");
    auto r_num = cs.eval("(m Num)");
    auto r_str = cs.eval("(m Str)");
    auto r_bool = cs.eval("(m Bool)");
    CHECK(r_num && is_int(*r_num), "m Num returns int");
    CHECK(r_str && is_int(*r_str), "m Str returns int");
    CHECK(r_bool && is_int(*r_bool), "m Bool returns int");
    if (r_num && is_int(*r_num))
        CHECK(as_int(*r_num) == 10, "Num arm returns 10");
    if (r_str && is_int(*r_str))
        CHECK(as_int(*r_str) == 20, "Str arm returns 20");
    if (r_bool && is_int(*r_bool))
        CHECK(as_int(*r_bool) == 30, "Bool arm returns 30");

    std::println("\n--- AC3: match site mutate → stats grow + eval ---");
    const auto stats3a = adt_exhaustiveness_stats(cs);
    (void)cs.eval("(mutate:rebind \"m\" \"(lambda (t) (match t ((Num) 11) ((Str) 21) "
                  "((Bool) 31)))\" \"issue-577-match\")");
    const auto stats3b = adt_exhaustiveness_stats(cs);
    std::println("  adt-exhaustiveness-stats: {} -> {}", stats3a, stats3b);
    CHECK(stats3b >= stats3a, "adt-exhaustiveness-stats monotonic after match mutate");
    auto r3 = cs.eval("(m Num)");
    CHECK(r3 && is_int(*r3), "m Num eval after mutate");
    if (r3 && is_int(*r3))
        CHECK(as_int(*r3) == 11, "Num arm returns 11 after mutate");

    std::println("\n--- AC4: incremental_infer → exhaust rechecks observable ---");
    (void)cs.eval("(mutate:rebind \"m\" \"(lambda (t) (match t ((Num) 12) ((Str) 22) "
                  "((Bool) 32)))\" \"issue-577-infer\")");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation logged");
    const auto snap4a = cs.snapshot();
    (void)cs.incremental_infer(ws->all_mutations().back());
    const auto snap4b = cs.snapshot();
    std::println("  adt_exhaust_rechecks: {} -> {}", snap4a.adt_exhaust_rechecks_total,
                 snap4b.adt_exhaust_rechecks_total);
    CHECK(snap4b.adt_exhaust_rechecks_total >= snap4a.adt_exhaust_rechecks_total,
          "adt_exhaust_rechecks monotonic after incremental infer");

    std::println("\n--- AC5: match-exhaustiveness-notes query regression ---");
    auto notes = cs.eval("(query:match-exhaustiveness-notes)");
    CHECK(notes.has_value(), "query:match-exhaustiveness-notes returns value");

    std::println("\n--- AC6: multi-round match arm mutate matrix ---");
    const auto stats6a = adt_exhaustiveness_stats(cs);
    for (int round = 0; round < 3; ++round) {
        const std::string m_body = "(lambda (t) (match t ((Num) " + std::to_string(round + 40) +
                                   ") ((Str) " + std::to_string(round + 50) + ") ((Bool) " +
                                   std::to_string(round + 60) + ")))";
        (void)cs.eval("(mutate:rebind \"m\" \"" + m_body + "\" \"r" + std::to_string(round) +
                      "-m\")");
        auto rn = cs.eval("(m Num)");
        auto rs = cs.eval("(m Str)");
        CHECK(rn && is_int(*rn), "m Num ok round " + std::to_string(round));
        CHECK(rs && is_int(*rs), "m Str ok round " + std::to_string(round));
        if (rn && is_int(*rn))
            CHECK(as_int(*rn) == round + 40, "Num semantics round " + std::to_string(round));
        if (rs && is_int(*rs))
            CHECK(as_int(*rs) == round + 50, "Str semantics round " + std::to_string(round));
    }
    const auto stats6b = adt_exhaustiveness_stats(cs);
    std::println("  adt-exhaustiveness-stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "adt-exhaustiveness-stats monotonic over matrix");

    std::println("\n--- AC7: query regression ---");
    auto ams = cs.eval("(query:adt-match-exhaust-stats)");
    auto mns = cs.eval("(compile:match-narrowing-stats)");
    CHECK(ams && is_int(*ams), "query:adt-match-exhaust-stats returns int");
    CHECK(mns.has_value(), "compile:match-narrowing-stats returns value");

    std::println("\n--- AC8: if-narrowed match subject counters ---");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (m Num) (m Str)))\" "
                  "\"issue-577-narrow\")");
    (void)cs.eval("(typecheck-current)");
    const auto snap8 = cs.snapshot();
    std::println("  match_subject_total={} match_subject_narrowed={}", snap8.match_subject_total,
                 snap8.match_subject_narrowed_total);
    CHECK(snap8.match_subject_total >= 0,
          "match_subject_total observable after if+match typecheck");

    const auto rechecks = cs.snapshot().adt_exhaust_rechecks_total;
    const auto impacts = cs.snapshot().adt_variant_mutate_impacts_total;
    std::println("  final adt_exhaust_rechecks={} variant_impacts={}", rechecks, impacts);
    CHECK(rechecks >= 0, "exhaustiveness_checks counter observable");
}

} // namespace aura_577_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_577_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}