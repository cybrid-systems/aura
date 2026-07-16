// test_typechecker_incremental_guard_steal_fidelity.cpp — Issue #798:
// ConstraintSystem incremental fidelity under Guard/steal/MutationBoundary
// (refines #792/#793/#466/#409; non-duplicative with #608/#509).
//
//   - AC1:  query:type-incremental-fidelity-stats reachable (schema 798)
//   - AC2:  cross-delta-blame-complete bumps on direct path
//   - AC3:  reverify-truncated-under-guard bumps on direct path
//   - AC4:  epoch-sync-hits bumps on direct path
//   - AC5:  blame-chain-length bumps on direct path
//   - AC6:  ConstraintSystem cross-delta blame + Guard epoch sync (production)
//   - AC7:  aggregate counters monotonic after bump matrix
//   - AC8:  query regression (#608 type-incremental, #509 constraint-delta)

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.type;

namespace aura_issue_798_detail {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::Evaluator;
using aura::compiler::SolveResult;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;

static std::int64_t stat_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t cross_delta_blame(CompilerService& cs) {
    return stat_int(cs, "cross-delta-blame-complete");
}
static std::int64_t reverify_truncated(CompilerService& cs) {
    return stat_int(cs, "reverify-truncated-under-guard");
}
static std::int64_t epoch_sync(CompilerService& cs) {
    return stat_int(cs, "epoch-sync-hits");
}
static std::int64_t blame_chain(CompilerService& cs) {
    return stat_int(cs, "blame-chain-length");
}

static SolveResult solve_delta_with(ConstraintSystem& cs, Constraint c) {
    cs.add_delta(std::move(c));
    return cs.solve_delta();
}

static void run_unit_ac6(CompilerService& cs) {
    std::println("\n--- AC6: ConstraintSystem cross-delta blame + Guard epoch sync ---");
    TypeRegistry reg;
    ConstraintSystem unit_cs(reg);
    CompilerMetrics metrics;
    unit_cs.set_metrics(&metrics);
    unit_cs.set_active_mutation_id(79801);
    const auto t = unit_cs.fresh_var();
    CHECK(solve_delta_with(unit_cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
          "T~Int delta establishes clean baseline");
    const auto v = unit_cs.fresh_var();
    unit_cs.mark_touched_on_delta(v, true);
    CHECK(solve_delta_with(unit_cs, {Constraint::EQUAL, v, reg.int_type()}) == SolveResult::SOLVED,
          "V~Int delta solves");
    CHECK(solve_delta_with(unit_cs, {Constraint::EQUAL, v, reg.string_type()}) ==
              SolveResult::CONFLICT,
          "V~String cross-delta CONFLICT with blame");
    CHECK(metrics.type_incremental_cross_delta_blame_complete_total.load() > 0,
          "cross_delta_blame_complete bumped on production path");
    CHECK(metrics.type_incremental_blame_chain_length_total.load() > 0,
          "blame_chain_length bumped on production path");

    const auto epoch0 = metrics.type_incremental_epoch_sync_hits_total.load();
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard guard(cs.evaluator(), &ok);
        const auto u = unit_cs.fresh_var();
        unit_cs.mark_touched_on_delta(u, true);
    }
    CHECK(metrics.type_incremental_epoch_sync_hits_total.load() > epoch0,
          "epoch_sync_hits bumped under MutationBoundaryGuard");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:type-incremental-fidelity-stats (schema 798) ---");
    auto h = cs.eval("(engine:metrics \"query:type-incremental-fidelity-stats\")");
    CHECK(h && is_hash(*h), "type-incremental-fidelity-stats returns hash");
    CHECK(stat_int(cs, "schema") == 798, "schema == 798");
    CHECK(cross_delta_blame(cs) >= 0, "cross-delta-blame-complete non-negative");
    CHECK(reverify_truncated(cs) >= 0, "reverify-truncated-under-guard non-negative");
    CHECK(epoch_sync(cs) >= 0, "epoch-sync-hits non-negative");
    CHECK(blame_chain(cs) >= 0, "blame-chain-length non-negative");

    std::println("\n--- AC2: cross-delta-blame-complete bumps on direct path ---");
    const auto b0 = cross_delta_blame(cs);
    cs.evaluator().bump_type_incremental_cross_delta_blame_complete(2);
    CHECK(cross_delta_blame(cs) == b0 + 2, "cross-delta-blame-complete bumps by 2");

    std::println("\n--- AC3: reverify-truncated-under-guard bumps on direct path ---");
    const auto r0 = reverify_truncated(cs);
    cs.evaluator().bump_type_incremental_reverify_truncated_under_guard();
    CHECK(reverify_truncated(cs) == r0 + 1, "reverify-truncated-under-guard bumps by 1");

    std::println("\n--- AC4: epoch-sync-hits bumps on direct path ---");
    const auto e0 = epoch_sync(cs);
    cs.evaluator().bump_type_incremental_epoch_sync_hits(2);
    CHECK(epoch_sync(cs) == e0 + 2, "epoch-sync-hits bumps by 2");

    std::println("\n--- AC5: blame-chain-length bumps on direct path ---");
    const auto c0 = blame_chain(cs);
    cs.evaluator().bump_type_incremental_blame_chain_length(3);
    CHECK(blame_chain(cs) == c0 + 3, "blame-chain-length bumps by 3");

    run_unit_ac6(cs);

    std::println("\n--- AC7: aggregate counters monotonic after bump matrix ---");
    const auto agg7a =
        cross_delta_blame(cs) + reverify_truncated(cs) + epoch_sync(cs) + blame_chain(cs);
    cs.evaluator().bump_type_incremental_cross_delta_blame_complete();
    cs.evaluator().bump_type_incremental_reverify_truncated_under_guard();
    cs.evaluator().bump_type_incremental_epoch_sync_hits();
    cs.evaluator().bump_type_incremental_blame_chain_length();
    const auto agg7b =
        cross_delta_blame(cs) + reverify_truncated(cs) + epoch_sync(cs) + blame_chain(cs);
    CHECK(agg7b >= agg7a + 4, "aggregate fidelity counters monotonic");

    std::println("\n--- AC8: query regression ---");
    auto inc608 = cs.eval("(engine:metrics \"query:type-incremental-stats\")");
    auto delta509 = cs.eval("(engine:metrics \"query:constraint-delta-stats\")");
    CHECK(inc608 && is_int(*inc608), "type-incremental-stats regression (#608)");
    CHECK(delta509 && is_int(*delta509), "constraint-delta-stats regression (#509)");
}

} // namespace aura_issue_798_detail

int aura_issue_typechecker_incremental_guard_steal_fidelity_run() {
    aura::compiler::CompilerService cs;
    aura_issue_798_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_typechecker_incremental_guard_steal_fidelity_run();
}
#endif
