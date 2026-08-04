// @category: unit
// @reason: Issue #2622 — single dirty-key authority for OccurrenceGoal +
//          predicate_memo (sync_occurrence_after_dirty + steal fence joint).
//
//   AC1: cond shape path + cache key miss wiring present
//   AC2: after sync, no live goal for invalidated cond
//   AC3: fence clears memo stale snapshot jointly with goal prune
//   AC4: empty affected → zero cost
//   AC5: schema-2622 additive
//   AC6: diverge stays 0 on ordered sync path

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
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
using aura::ast::NodeId;
using aura::ast::StringPool;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::ConstraintSystem;
using aura::compiler::InferenceEngine;
using aura::compiler::TypeChecker;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeId;
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

// ── AC1: shape / miss wiring ──
static void ac1_shape_miss_refresh() {
    std::println("\n--- #2622 AC1: structural key miss path wired ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(ixx.find("sync_occurrence_after_dirty") != std::string::npos, "AC1: API present");
    CHECK(impl.find("sync_occurrence_after_dirty") != std::string::npos, "AC1: impl present");
    CHECK(impl.find("cond_shape_hash") != std::string::npos, "AC1: shape hash lineage");
    CHECK(impl.find("occurrence_cache_key_misses_") != std::string::npos ||
              impl.find("occurrence_cache_key_miss") != std::string::npos,
          "AC1: cache key miss counter");
    CHECK(impl.find("Issue #2622") != std::string::npos, "AC1: cites #2622");
    // Shape hash changes under same NodeId still produce different hashes.
    FlatAST flat;
    StringPool pool;
    auto x = flat.add_variable(pool.intern("x"));
    const auto h0 = TypeChecker::hash_node_shape(flat, x, 0);
    auto y = flat.add_variable(pool.intern("y"));
    CHECK(TypeChecker::hash_node_shape(flat, y, 0) != h0, "AC1: name change → shape change");
}

// ── AC2: no live goal after sync ──
static void ac2_no_live_goal_after_sync() {
    std::println("\n--- #2622 AC2: sync drops goals for dirty cond ---");
    TypeRegistry reg;
    DiagnosticCollector diag;
    InferenceEngine eng(reg, diag);
    eng.set_cache_epoch(1);
    ConstraintSystem long_cs(reg);
    long_cs.set_current_epoch(1);

    // Seed a goal for cond node 42.
    TypeId var{}, refined{};
    var.index = 1;
    refined.index = 2;
    eng.constraint_system().note_occurrence_goal(var, refined, /*pred=*/42, /*mut=*/7, /*epoch=*/1);
    long_cs.note_occurrence_goal(var, refined, 42, 7, 1);
    CHECK(eng.constraint_system().occurrence_goals_size() == 1, "AC2: engine goal seeded");
    CHECK(long_cs.occurrence_goals_size() == 1, "AC2: long-lived goal seeded");

    // Dirty that cond (as NodeId).
    std::vector<NodeId> affected{static_cast<NodeId>(42)};
    const auto dropped = eng.sync_occurrence_after_dirty(affected, /*flat=*/nullptr, &long_cs);
    CHECK(dropped >= 1, "AC2: at least one goal dropped");
    CHECK(eng.constraint_system().occurrence_goals_size() == 0, "AC2: engine goals empty");
    CHECK(long_cs.occurrence_goals_size() == 0, "AC2: long-lived goals empty");
    // No goal remains for cond 42.
    for (const auto& g : eng.constraint_system().occurrence_goals_for_test())
        CHECK(g.predicate_cond_node != 42, "AC2: no residual goal for cond 42");
}

// ── AC3: fence joint ──
static void ac3_fence_joint_memo() {
    std::println("\n--- #2622 AC3: fence clears memo stale jointly ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    CHECK(ixx.find("last_predicate_memo_stale_vs_epoch_ = 0") != std::string::npos,
          "AC3: fence clears stale snapshot");
    CHECK(ixx.find("occurrence_memo_goal_fence_joint_total") != std::string::npos,
          "AC3: fence joint metric");
    CHECK(ixx.find("Issue #2622") != std::string::npos, "AC3: cites #2622 on fence");

    TypeRegistry reg;
    TypeChecker tc(reg);
    CompilerMetrics metrics;
    tc.set_metrics(&metrics);
    // Advance epoch via fence → goals prune path + joint clear.
    (void)tc.note_steal_or_densify_epoch_fence(1);
    CHECK(tc.last_predicate_memo_stale_vs_epoch() == 0, "AC3: stale snapshot 0 after fence");
    CHECK(metrics.occurrence_memo_goal_fence_joint_total.load() >= 1, "AC3: joint counter");
    // Same epoch zero cost.
    const auto j0 = metrics.occurrence_memo_goal_fence_joint_total.load();
    CHECK(tc.note_steal_or_densify_epoch_fence(1) == 0, "AC3: same epoch zero drop");
    CHECK(metrics.occurrence_memo_goal_fence_joint_total.load() == j0,
          "AC3: same epoch no joint bump");
}

// ── AC4: empty zero cost ──
static void ac4_empty_zero_cost() {
    std::println("\n--- #2622 AC4: empty affected zero cost ---");
    TypeRegistry reg;
    DiagnosticCollector diag;
    InferenceEngine eng(reg, diag);
    const auto d0 = eng.occurrence_sync_after_dirty_total();
    const auto g0 = eng.constraint_system().occurrence_goals_size();
    std::vector<NodeId> empty;
    CHECK(eng.sync_occurrence_after_dirty(empty, nullptr, nullptr) == 0, "AC4: returns 0");
    CHECK(eng.occurrence_sync_after_dirty_total() == d0, "AC4: no sync counter bump");
    CHECK(eng.constraint_system().occurrence_goals_size() == g0, "AC4: goals unchanged");
    // drop API empty
    CHECK(eng.constraint_system().drop_occurrence_goals_for_conds({}) == 0, "AC4: drop empty 0");
}

// ── AC5: schema ──
static void ac5_schema_source() {
    std::println("\n--- #2622 AC5: schema-2622 additive ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2622") == 2622, "AC5: schema-2622");
    CHECK(href(cs, "issue-2622") == 2622, "AC5: issue-2622");
    CHECK(href(cs, "occurrence-memo-goal-diverge-total") >= 0, "AC5: diverge key");
    CHECK(href(cs, "occurrence-sync-after-dirty-total") >= 0, "AC5: sync key");
    CHECK(href(cs, "occurrence-memo-goal-fence-joint-total") >= 0, "AC5: fence joint key");
    CHECK(href(cs, "occurrence-dirty-key-authority-wired") == 1, "AC5: wired");
    CHECK(href(cs, "schema-2461") == 2461, "AC5: schema-2461 retained");
    const auto met = read_file("src/compiler/observability_metrics.h");
    CHECK(met.find("occurrence_memo_goal_diverge_total") != std::string::npos,
          "AC5: metrics field");
}

// ── AC6: ordered path diverge 0 ──
static void ac6_diverge_zero_ordered() {
    std::println("\n--- #2622 AC6: ordered sync diverge stays 0 ---");
    TypeRegistry reg;
    DiagnosticCollector diag;
    InferenceEngine eng(reg, diag);
    eng.set_cache_epoch(1);
    TypeId var{}, refined{};
    var.index = 3;
    refined.index = 4;
    eng.constraint_system().note_occurrence_goal(var, refined, 99, 1, 1);
    // No memo entry → no diverge on sync.
    std::vector<NodeId> affected{static_cast<NodeId>(99)};
    const auto div0 = eng.occurrence_memo_goal_diverge_total();
    (void)eng.sync_occurrence_after_dirty(affected, nullptr, nullptr);
    CHECK(eng.occurrence_memo_goal_diverge_total() == div0, "AC6: diverge unchanged (0)");
    CHECK(eng.constraint_system().occurrence_goals_size() == 0, "AC6: goals dropped");
    CHECK(eng.occurrence_sync_after_dirty_total() >= 1, "AC6: sync total advanced");
}

} // namespace

int run_test_occurrence_dirty_key_authority_2622() {
    std::println("=== Issue #2622: occurrence dirty-key authority ===");
    ac1_shape_miss_refresh();
    ac2_no_live_goal_after_sync();
    ac3_fence_joint_memo();
    ac4_empty_zero_cost();
    ac5_schema_source();
    ac6_diverge_zero_ordered();
    std::println("\n=== #2622: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_occurrence_dirty_key_authority_2622();
}
#endif
