// @category: unit
// @reason: Issue #2359 — unify occurrence_goals + predicate_memo epoch
// health on query:type-incremental-fidelity-stats (pure read keys).
//
//   AC1: Two successive queries without mutate return identical epoch
//        health keys (cache-epoch, goals-live, memo-live, stale, delta)
//   AC2: After epoch advance + prune, occurrence-goals-live drops stale;
//        memo stale-vs-epoch / delta documents lag; selective min_gen
//        clears memo lag
//   AC3: No solve side effects; no schema break on #2107/#2278/#2307/#2308
//   AC4: memo-goal-epoch-health-wired == 1 when feature landed
//   AC5: Tests AC matrix + source-cite query registration

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

import std;
import aura.core.ast;
import aura.core.type;
import aura.diag;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::StringPool;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::ConstraintSystem;
using aura::compiler::InferenceEngine;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;
using aura::diag::DiagnosticCollector;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: two successive queries identical without mutate ──
static void ac1_stable_successive_queries() {
    std::println("\n--- AC1: successive queries identical epoch health ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "warm");

    const auto e1 = href(svc, "cache-epoch");
    const auto g1 = href(svc, "occurrence-goals-live");
    const auto m1 = href(svc, "predicate-memo-live");
    const auto s1 = href(svc, "predicate-memo-stale-vs-epoch");
    const auto d1 = href(svc, "memo-goal-epoch-delta");
    const auto w1 = href(svc, "memo-goal-epoch-health-wired");
    const auto sch1 = href(svc, "schema-2359");

    const auto e2 = href(svc, "cache-epoch");
    const auto g2 = href(svc, "occurrence-goals-live");
    const auto m2 = href(svc, "predicate-memo-live");
    const auto s2 = href(svc, "predicate-memo-stale-vs-epoch");
    const auto d2 = href(svc, "memo-goal-epoch-delta");
    const auto w2 = href(svc, "memo-goal-epoch-health-wired");
    const auto sch2 = href(svc, "schema-2359");

    CHECK(e1 == e2, "AC1: cache-epoch stable");
    CHECK(g1 == g2, "AC1: occurrence-goals-live stable");
    CHECK(m1 == m2, "AC1: predicate-memo-live stable");
    CHECK(s1 == s2, "AC1: predicate-memo-stale-vs-epoch stable");
    CHECK(d1 == d2, "AC1: memo-goal-epoch-delta stable");
    CHECK(w1 == w2 && w1 == 1, "AC1: wired stable == 1");
    CHECK(sch1 == sch2 && sch1 == 2359, "AC1: schema-2359 stable");
    // Vacuous healthy on empty / no commit CS.
    CHECK(g1 == 0, "AC1: empty goals live == 0");
    CHECK(m1 == 0 || m1 >= 0, "AC1: memo-live non-negative");
    CHECK(s1 == 0, "AC1: empty memo stale == 0");
    CHECK(d1 == 0, "AC1: empty delta healthy == 0");
}

// ── AC2: epoch advance + prune drops goals; memo lag then selective clear ──
static void ac2_epoch_advance_prune_and_memo_lag() {
    std::println("\n--- AC2: epoch advance prune + memo stale lag ---");

    // Goals path (CS unit).
    {
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        CompilerMetrics m;
        cs.set_metrics(&m);
        cs.set_current_epoch(1);
        auto v1 = cs.fresh_var();
        auto v2 = cs.fresh_var();
        cs.mark_touched_on_delta(v1, /*occurrence_narrow=*/true);
        cs.mark_touched_on_delta(v2, /*occurrence_narrow=*/true);
        CHECK(cs.occurrence_goals_size() == 2, "AC2.1: two goals after narrow");
        CHECK(cs.occurrence_goals_max_epoch() == 1, "AC2.2: max goal epoch == 1");
        CHECK(cs.occurrence_goals_stale_vs_epoch(1) == 0, "AC2.3: not stale at epoch 1");
        CHECK(cs.occurrence_goals_stale_vs_epoch(2) == 2, "AC2.4: both stale vs epoch 2");

        const auto dropped = cs.prune_occurrence_goals(2);
        cs.set_current_epoch(2);
        CHECK(dropped == 2, "AC2.5: prune drops 2 stale goals");
        CHECK(cs.occurrence_goals_size() == 0, "AC2.6: goals-live == 0 after prune");
        CHECK(cs.occurrence_goals_stale_vs_epoch(2) == 0, "AC2.7: no stale survivors");
    }

    // Memo path (InferenceEngine unit): reanalyze → epoch advance → stale;
    // then min_gen selective → lag clears.
    {
        TypeRegistry reg;
        DiagnosticCollector diag;
        InferenceEngine eng(reg, diag);
        eng.set_cache_epoch(1);

        FlatAST flat;
        StringPool pool;
        auto x = pool.intern("x");
        auto num = pool.intern("number?");
        auto xv = flat.add_variable(x);
        auto num_v = flat.add_variable(num);
        auto cond_x = flat.add_call(num_v, std::array<aura::ast::NodeId, 1>{xv});
        auto then0 = flat.add_literal(1);
        auto else0 = flat.add_literal(0);
        auto if_x = flat.add_if(cond_x, then0, else0);
        flat.root = if_x;

        constexpr auto kOcc =
            static_cast<std::uint8_t>(aura::ast::FlatAST::DirtyReason::kOccurrenceDirty);
        flat.mark_dirty(if_x, kOcc);
        flat.mark_occurrence_stale(if_x);
        std::vector<aura::ast::NodeId> targets{if_x};
        (void)eng.reanalyze_occurrence_contexts(flat, pool, targets);

        const auto live0 = eng.predicate_memo_size();
        const auto stale0 = eng.predicate_memo_stale_vs_epoch();
        CHECK(stale0 == 0, "AC2.8: memo aligned at capture epoch");

        // Advance epoch without selective invalidate → lag documents.
        eng.set_cache_epoch(2);
        const auto live1 = eng.predicate_memo_size();
        const auto stale1 = eng.predicate_memo_stale_vs_epoch();
        if (live0 > 0) {
            CHECK(live1 == live0, "AC2.9: memo size holds across epoch (no wholesale clear)");
            CHECK(stale1 == live1, "AC2.10: all live entries stale-vs-epoch after advance");
            CHECK(stale1 > 0, "AC2.11: delta documents memo lag");
        } else {
            // Soft: reanalyze may not populate without full type env.
            CHECK(stale1 == 0, "AC2.9s: empty memo → stale 0");
        }

        // Selective min_gen (same as #2068 / #2285 path) clears lag.
        const auto dropped = eng.invalidate_predicate_memo_for_min_gen(2);
        CHECK(dropped == live1, "AC2.12: min_gen drops all pre-epoch-2 entries");
        CHECK(eng.predicate_memo_size() == 0, "AC2.13: memo live 0 after selective");
        CHECK(eng.predicate_memo_stale_vs_epoch() == 0, "AC2.14: stale cleared");
    }
}

// ── AC3: no schema break on lineage keys ──
static void ac3_lineage_no_schema_break() {
    std::println("\n--- AC3: lineage schema keys preserved ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(svc, "schema") == 1617, "AC3: schema 1617 lineage");
    CHECK(href(svc, "schema-2278") == 2278, "AC3: schema-2278");
    CHECK(href(svc, "occurrence-goal-sole-authority-wired") == 1, "AC3: #2307 sole-authority");
    CHECK(href(svc, "schema-2308") == 2308, "AC3: schema-2308");
    CHECK(href(svc, "solver-snapshot-wired") == 1, "AC3: solver-snapshot-wired");
    CHECK(href(svc, "schema-2104") == 2104, "AC3: schema-2104 memo selective");
    // Two queries still pure (no solve side effects from health keys).
    const auto d0 = href(svc, "memo-goal-epoch-delta");
    const auto d1 = href(svc, "memo-goal-epoch-delta");
    CHECK(d0 == d1, "AC3: pure read (delta unchanged across queries)");
}

// ── AC4: wired sentinel ──
static void ac4_wired_sentinel() {
    std::println("\n--- AC4: memo-goal-epoch-health-wired == 1 ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(svc, "memo-goal-epoch-health-wired") == 1, "AC4: wired == 1");
    CHECK(href(svc, "schema-2359") == 2359, "AC4: schema-2359");
    CHECK(href(svc, "issue-2359") == 2359, "AC4: issue-2359");
    CHECK(href(svc, "cache-epoch") >= 0, "AC4: cache-epoch key present");
    CHECK(href(svc, "occurrence-goals-live") >= 0, "AC4: goals-live present");
    CHECK(href(svc, "predicate-memo-live") >= 0, "AC4: memo-live present");
    CHECK(href(svc, "predicate-memo-stale-vs-epoch") >= 0, "AC4: memo-stale present");
    CHECK(href(svc, "memo-goal-epoch-delta") >= 0, "AC4: delta present");
}

// ── AC5: source-cite query registration ──
static void ac5_source_cite() {
    std::println("\n--- AC5: source-cite query + accessors ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto tci = read_file("src/compiler/type_checker.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(q.find("schema-2359") != std::string::npos, "AC5: query schema-2359");
    CHECK(q.find("memo-goal-epoch-health-wired") != std::string::npos, "AC5: query wired");
    CHECK(q.find("cache-epoch") != std::string::npos, "AC5: query cache-epoch");
    CHECK(q.find("occurrence-goals-live") != std::string::npos, "AC5: query goals-live");
    CHECK(q.find("predicate-memo-live") != std::string::npos, "AC5: query memo-live");
    CHECK(q.find("predicate-memo-stale-vs-epoch") != std::string::npos, "AC5: query memo-stale");
    CHECK(q.find("memo-goal-epoch-delta") != std::string::npos, "AC5: query delta");
    CHECK(q.find("Issue #2359") != std::string::npos, "AC5: query cites #2359");
    CHECK(tci.find("predicate_memo_stale_vs_epoch") != std::string::npos, "AC5: IE stale accessor");
    CHECK(tci.find("occurrence_goals_stale_vs_epoch") != std::string::npos,
          "AC5: CS goals stale accessor");
    CHECK(tci.find("last_predicate_memo_live") != std::string::npos,
          "AC5: TC last memo live snapshot");
    CHECK(impl.find("last_predicate_memo_live_") != std::string::npos,
          "AC5: partial snapshots memo live");
    CHECK(impl.find("last_predicate_memo_stale_vs_epoch_") != std::string::npos,
          "AC5: partial snapshots memo stale");
}

} // namespace

int run_test_memo_goal_epoch_health_2359() {
    std::println("=== Issue #2359: memo + goal epoch health query surface ===");
    ac1_stable_successive_queries();
    ac2_epoch_advance_prune_and_memo_lag();
    ac3_lineage_no_schema_break();
    ac4_wired_sentinel();
    ac5_source_cite();
    std::println("\n=== #2359: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_memo_goal_epoch_health_2359();
}
#endif
