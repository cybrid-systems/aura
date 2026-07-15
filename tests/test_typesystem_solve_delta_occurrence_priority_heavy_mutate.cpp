// test_typesystem_solve_delta_occurrence_priority_heavy_mutate.cpp — Issue #745:
// Dynamic reverify limit + Occurrence-narrowed priority in solve_delta under
// heavy structural typed mutation (50+ nested predicates).
//
// Non-duplicative with #466, #690, #659, #674.
//
//   - AC1: query:constraint-reverify-occurrence-stats (schema 745)
//   - AC2: unit occurrence-priority reverify + blame chain
//   - AC3: 50+ predicate define + mutate:rebind structural stress
//   - AC4: no TIMEOUT / eval correctness after heavy mutate
//   - AC5: occurrence-reverify stats monotonic over matrix
//   - AC6: query regression (constraint-typed-mutate-stats)
//   - AC7: multi-round mutate + typecheck stress (#674)

#include "observability_metrics.h"
#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.type;

namespace aura_issue_745_detail {

using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::SolveResult;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t occ_hash(CompilerService& cs, const std::string& key) {
    auto r =
        cs.eval("(hash-ref (engine:metrics \"query:constraint-reverify-occurrence-stats\") \"" +
                key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t occ_stats_sum(CompilerService& cs) {
    const auto narrow = occ_hash(cs, "reverify-hits-on-narrow");
    const auto blame = occ_hash(cs, "cross-delta-blame-complete");
    const auto timeout = occ_hash(cs, "timeout-prevented");
    const auto stale = occ_hash(cs, "stale-blame-invalidation");
    if (narrow < 0 || blame < 0 || timeout < 0 || stale < 0)
        return -1;
    return narrow + blame + timeout + stale;
}

static std::string build_deep_predicate_lambda(int nest_count) {
    std::string cond = "(number? x)";
    for (int i = 0; i < nest_count; ++i) {
        if (i % 2 == 0)
            cond = "(and " + cond + " (not (string? x)))";
        else
            cond = "(or " + cond + " (not (pair? x)))";
    }
    return "(lambda (x) (if " + cond + " (+ x 1) 0))";
}

static std::string build_workspace(int nest_count) {
    std::string body = build_deep_predicate_lambda(nest_count);
    return std::format("(define deep-pred {})\n(define aux (lambda (y) (deep-pred y)))", body);
}

static SolveResult solve_delta_with(ConstraintSystem& cs, Constraint c) {
    cs.add_delta(std::move(c));
    return cs.solve_delta();
}

static void run_unit_ac2() {
    std::println("\n--- AC2: unit occurrence-priority reverify + blame ---");
    aura::core::TypeRegistry reg;
    ConstraintSystem unit_cs(reg);
    aura::compiler::CompilerMetrics metrics;
    unit_cs.set_metrics(&metrics);
    unit_cs.set_active_mutation_id(74501);
    const auto t = unit_cs.fresh_var();
    CHECK(solve_delta_with(unit_cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
          "T~Int delta establishes clean baseline constraint");
    unit_cs.mark_touched_on_delta(t, true);
    const auto u = unit_cs.fresh_var();
    const auto before_narrow = metrics.constraint_reverify_narrow_hits_total.load();
    CHECK(solve_delta_with(unit_cs, {Constraint::EQUAL, u, reg.int_type()}) == SolveResult::SOLVED,
          "delta U~Int solves and triggers occurrence-priority reverify scan");
    const auto after_narrow = metrics.constraint_reverify_narrow_hits_total.load();
    CHECK(after_narrow > before_narrow, "reverify_hits_on_narrow bumped with priority scan");
    const auto v = unit_cs.fresh_var();
    unit_cs.mark_touched_on_delta(v, true);
    CHECK(solve_delta_with(unit_cs, {Constraint::EQUAL, v, reg.int_type()}) == SolveResult::SOLVED,
          "V~Int solves");
    CHECK(solve_delta_with(unit_cs, {Constraint::EQUAL, v, reg.string_type()}) ==
              SolveResult::CONFLICT,
          "V~String cross-delta CONFLICT with blame");
    CHECK(metrics.constraint_blame_chain_complete_total.load() > 0,
          "cross_delta_blame_complete with active_mutation_id");
    CHECK(metrics.delta_conflict_reverify_total.load() > 0, "delta_conflict_reverify_total bumped");
    CHECK(metrics.delta_conflict_detected_total.load() > 0, "delta_conflict_detected_total bumped");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:constraint-reverify-occurrence-stats (schema 745) ---");
    auto h = cs.eval("(engine:metrics \"query:constraint-reverify-occurrence-stats\")");
    CHECK(h && is_hash(*h), "constraint-reverify-occurrence-stats returns hash");
    CHECK(occ_hash(cs, "schema") == 745, "schema == 745");
    CHECK(occ_hash(cs, "reverify-hits-on-narrow") >= 0, "reverify-hits-on-narrow present");
    CHECK(occ_hash(cs, "cross-delta-blame-complete") >= 0, "cross-delta-blame-complete present");
    CHECK(occ_hash(cs, "timeout-prevented") >= 0, "timeout-prevented present");
    CHECK(occ_hash(cs, "stale-blame-invalidation") >= 0, "stale-blame-invalidation present");

    std::println("\n--- AC3: 50+ nested predicate workspace + heavy mutate ---");
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Lazy);
    const auto workspace = build_workspace(52);
    const auto escaped = [&workspace] {
        std::string out;
        out.reserve(workspace.size() + 16);
        for (char c : workspace) {
            if (c == '\\' || c == '"')
                out.push_back('\\');
            out.push_back(c);
        }
        return out;
    }();
    CHECK(cs.eval(std::format("(set-code \"{}\")", escaped)).has_value(),
          "load 52-nest predicate workspace");
    CHECK(cs.eval("(eval-current)").has_value(), "eval deep-predicate workspace");
    CHECK(cs.eval("(typecheck-current)").has_value(), "typecheck deep-predicate workspace");
    auto r0 = cs.eval("(deep-pred 5)");
    CHECK(r0 && is_int(*r0) && as_int(*r0) == 6, "deep-pred 5 == 6 baseline");

    const auto stats3a = occ_stats_sum(cs);
    const auto mutate_body = build_deep_predicate_lambda(52);
    std::string escaped_mutate;
    escaped_mutate.reserve(mutate_body.size() + 16);
    for (char c : mutate_body) {
        if (c == '\\' || c == '"')
            escaped_mutate.push_back('\\');
        escaped_mutate.push_back(c);
    }
    auto reb =
        cs.eval(std::format("(mutate:rebind \"deep-pred\" \"{}\" \"issue-745\")", escaped_mutate));
    CHECK(reb && aura::compiler::types::is_bool(*reb) && aura::compiler::types::as_bool(*reb),
          "mutate:rebind on 52-nest predicate succeeds");
    CHECK(cs.eval("(eval-current)").has_value(), "eval after heavy mutate");
    CHECK(cs.eval("(typecheck-current)").has_value(), "typecheck after heavy mutate (no TIMEOUT)");

    std::println("\n--- AC4: eval correctness post heavy mutate ---");
    auto r1 = cs.eval("(deep-pred 10)");
    CHECK(r1 && is_int(*r1) && as_int(*r1) == 11, "deep-pred 10 == 11 after mutate");
    auto r2 = cs.eval("(aux 3)");
    CHECK(r2 && is_int(*r2) && as_int(*r2) == 4, "aux 3 == 4 via deep-pred alias");

    std::println("\n--- AC5: occurrence-reverify stats monotonic ---");
    const auto stats3b = occ_stats_sum(cs);
    std::println("  occurrence-reverify sum: {} -> {}", stats3a, stats3b);
    CHECK(stats3b >= stats3a, "occurrence-reverify stats monotonic after heavy mutate");

    std::println("\n--- AC6: query regression ---");
    auto ctm = cs.eval("(engine:metrics \"query:constraint-typed-mutate-stats\")");
    CHECK(ctm && is_hash(*ctm), "constraint-typed-mutate-stats regression");
    auto count = cs.eval("(stats:count)");
    CHECK(count && is_int(*count) && as_int(*count) > 0, "stats:count positive");

    std::println("\n--- AC7: multi-round mutate matrix (#674 stress) ---");
    const auto stats7a = occ_stats_sum(cs);
    for (int round = 0; round < 4; ++round) {
        const auto alt = build_deep_predicate_lambda(50 + round);
        std::string esc;
        for (char c : alt) {
            if (c == '\\' || c == '"')
                esc.push_back('\\');
            esc.push_back(c);
        }
        (void)cs.eval(std::format("(mutate:rebind \"deep-pred\" \"{}\" \"r{}-745\")", esc, round));
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(typecheck-current)");
        auto rv = cs.eval("(deep-pred 2)");
        CHECK(rv && is_int(*rv), "deep-pred eval ok round " + std::to_string(round));
    }
    const auto stats7b = occ_stats_sum(cs);
    std::println("  occurrence-reverify sum: {} -> {}", stats7a, stats7b);
    CHECK(stats7b >= stats7a, "occurrence-reverify stats monotonic over stress matrix");
}

} // namespace aura_issue_745_detail

int aura_issue_typesystem_solve_delta_occurrence_priority_heavy_mutate_run() {
    using namespace aura_issue_745_detail;
    std::println("=== Issue #745: solve_delta occurrence priority + heavy mutate ===");
    run_unit_ac2();
    CompilerService cs;
    run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_typesystem_solve_delta_occurrence_priority_heavy_mutate_run();
}
#endif
