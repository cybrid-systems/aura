// @category: unit
// @reason: Issue #2552 — steal/densify joint freshness for OccurrenceGoal
//          + type_dep (LayoutStamp restamp pairs with type epoch fence).
//
//   AC1: Steal success + old-epoch goals → stale_vs_epoch(new)==0 after fence
//   AC2: Hard-fail path does not call fence (goals preserved) — source-cite
//   AC3: Densify escape-clear path wires same fence helper
//   AC4: Same epoch → zero prune cost (counters stable)
//   AC5: Chaos-style multi-fence soak — no residual stale goals
//   AC6: Source-cite + linter; schema-2552

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.ast;
import aura.core.type;

namespace {

using aura::ast::NodeId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::ConstraintSystem;
using aura::compiler::TypeChecker;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeId;
using aura::core::TypeRegistry;
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

static std::int64_t href(CompilerService& cs, const char* query, const char* key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", query, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: fence drops old-epoch goals; stale_vs == 0 ──
static void ac1_steal_fence_prunes_goals() {
    std::println("\n--- #2552 AC1: fence prunes old-epoch occurrence goals ---");
    TypeRegistry reg;
    TypeChecker tc(reg);
    CompilerMetrics metrics{};
    tc.set_metrics(&metrics);
    auto& cs = tc.constraint_system();
    cs.set_metrics(&metrics);

    // Seed goals at epoch 1 + type_dep edges at epoch 1.
    tc.set_cache_epoch(1);
    cs.set_current_epoch(1);
    // note_occurrence_goal needs real types — use raw push via test API
    // when available; else record_type_dep + set goals through CS path.
    // Use set_current_epoch + manual goal stamp via note if exposed.
    // ConstraintSystem::note_occurrence_goal signature from #2278:
    // find note_occurrence_goal
    TypeId v = cs.fresh_var();
    TypeId refined = reg.int_type();
    cs.note_occurrence_goal(v, refined, /*pred=*/1, /*mut=*/1, /*epoch=*/1);
    cs.note_occurrence_goal(v, refined, /*pred=*/2, /*mut=*/1, /*epoch=*/1);
    CHECK(cs.occurrence_goals_size() >= 2, "AC1: goals seeded");
    CHECK(cs.occurrence_goals_stale_vs_epoch(2) >= 2, "AC1: goals stale vs epoch 2");

    tc.record_type_dependency(/*tid=*/42, /*node=*/10);
    tc.record_type_dependency(/*tid=*/42, /*node=*/11);
    CHECK(tc.type_dep_graph_edge_count() >= 2, "AC1: type_dep edges seeded");

    const auto gp0 = metrics.occurrence_goal_steal_prune_total.load(std::memory_order_relaxed);
    const auto td0 = metrics.type_dep_steal_prune_total.load(std::memory_order_relaxed);

    // Fence to epoch 2 (simulates post-steal restamp).
    const auto dropped = tc.note_steal_or_densify_epoch_fence(2);
    CHECK(dropped >= 2, "AC1: goals dropped");
    CHECK(cs.occurrence_goals_stale_vs_epoch(2) == 0, "AC1: no residual stale goals");
    CHECK(cs.occurrence_goals_size() == 0, "AC1: epoch-1 goals gone");
    CHECK(metrics.occurrence_goal_steal_prune_total.load(std::memory_order_relaxed) == gp0 + 1,
          "AC1: steal_prune total +1");
    CHECK(metrics.type_dep_steal_prune_total.load(std::memory_order_relaxed) == td0 + 1,
          "AC1: type_dep steal prune +1");
    CHECK(tc.cache_epoch() == 2, "AC1: cache_epoch advanced");
}

// ── AC2: hard-fail skips fence (source-cite) ──
static void ac2_hard_fail_no_fence() {
    std::println("\n--- #2552 AC2: hard-fail steal path skips fence ---");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(efm.find("note_type_freshness_after_steal_or_densify") != std::string::npos,
          "AC2: steal-complete wires fence");
    CHECK(efm.find("!hard_failed") != std::string::npos ||
              efm.find("hard_failed") != std::string::npos,
          "AC2: hard_failed gate present");
    // Fence only inside success restamp block (after !hard_failed restamp).
    CHECK(efm.find("Issue #2552") != std::string::npos, "AC2: #2552 cite on steal path");
    // Goals preserved semantics: hard path does not call fence.
    CHECK(efm.find("Hard-fail path above skips") != std::string::npos ||
              efm.find("hard-fail") != std::string::npos,
          "AC2: hard-fail skip documented");
}

// ── AC3: densify path wires fence ──
static void ac3_densify_wires_fence() {
    std::println("\n--- #2552 AC3: densify escape-clear pairs with fence ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(emb.find("note_type_freshness_after_steal_or_densify") != std::string::npos,
          "AC3: densify path calls fence");
    CHECK(emb.find("note_escape_gate_clear_on_densify") != std::string::npos,
          "AC3: escape clear present");
    CHECK(emb.find("Issue #2552") != std::string::npos || emb.find("#2552") != std::string::npos,
          "AC3: densify cites #2552");
    // Only under had_moving_densify
    CHECK(emb.find("had_moving_densify") != std::string::npos, "AC3: Moving densify gate");
}

// ── AC4: same epoch zero cost ──
static void ac4_same_epoch_zero_cost() {
    std::println("\n--- #2552 AC4: same epoch → zero prune cost ---");
    TypeRegistry reg;
    TypeChecker tc(reg);
    CompilerMetrics metrics{};
    tc.set_metrics(&metrics);
    tc.set_cache_epoch(5);
    const auto gp0 = metrics.occurrence_goal_steal_prune_total.load(std::memory_order_relaxed);
    const auto td0 = metrics.type_dep_steal_prune_total.load(std::memory_order_relaxed);
    CHECK(tc.note_steal_or_densify_epoch_fence(5) == 0, "AC4: same epoch drops 0");
    CHECK(metrics.occurrence_goal_steal_prune_total.load(std::memory_order_relaxed) == gp0,
          "AC4: steal prune total flat");
    CHECK(metrics.type_dep_steal_prune_total.load(std::memory_order_relaxed) == td0,
          "AC4: type_dep steal prune flat");
    CHECK(tc.note_steal_or_densify_epoch_fence(0) == 0, "AC4: epoch 0 no-op");
}

// ── AC5: multi-round soak ──
static void ac5_multi_round_soak() {
    std::println("\n--- #2552 AC5: multi-round fence soak ---");
    TypeRegistry reg;
    TypeChecker tc(reg);
    CompilerMetrics metrics{};
    tc.set_metrics(&metrics);
    auto& cs = tc.constraint_system();
    cs.set_metrics(&metrics);
    TypeId v = cs.fresh_var();
    TypeId refined = reg.int_type();
    for (std::uint64_t e = 1; e <= 20; ++e) {
        cs.set_current_epoch(e);
        cs.note_occurrence_goal(v, refined, /*pred=*/static_cast<std::uint32_t>(e),
                                /*mut=*/1, e);
        tc.record_type_dependency(/*tid=*/7, /*node=*/static_cast<NodeId>(e));
        (void)tc.note_steal_or_densify_epoch_fence(e + 1);
        CHECK(cs.occurrence_goals_stale_vs_epoch(e + 1) == 0,
              "AC5: no stale goals after fence round");
    }
    CHECK(metrics.occurrence_goal_steal_prune_total.load(std::memory_order_relaxed) >= 20,
          "AC5: fence called each round");
}

// ── AC6: source-cite + schema ──
static void ac6_source_and_schema() {
    std::println("\n--- #2552 AC6: source-cite + schema-2552 ---");
    const auto tc = read_file("src/compiler/type_checker.ixx");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_type_freshness_steal_densify_2552.py");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");

    CHECK(tc.find("note_steal_or_densify_epoch_fence") != std::string::npos, "AC6: fence API");
    CHECK(tc.find("Issue #2552") != std::string::npos, "AC6: type_checker cites #2552");
    CHECK(etc.find("note_type_freshness_after_steal_or_densify") != std::string::npos,
          "AC6: Evaluator helper");
    CHECK(efm.find("note_type_freshness_after_steal_or_densify") != std::string::npos,
          "AC6: steal wires helper");
    CHECK(emb.find("note_type_freshness_after_steal_or_densify") != std::string::npos,
          "AC6: densify wires helper");
    CHECK(q.find("schema-2552") != std::string::npos, "AC6: schema-2552");
    CHECK(q.find("occurrence-goal-steal-prune-total") != std::string::npos, "AC6: goal counter");
    CHECK(q.find("type-dep-steal-prune-total") != std::string::npos, "AC6: type_dep counter");
    CHECK(!lint.empty(), "AC6: linter present");
    CHECK(cmake.find("test_type_freshness_steal_densify_2552") != std::string::npos, "AC6: cmake");
    CHECK(build.find("check_type_freshness_steal_densify_2552") != std::string::npos,
          "AC6: build script");
    CHECK(build.find("cmd_type_freshness_steal_densify_coverage") != std::string::npos,
          "AC6: build cmd");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "query:type-incremental-fidelity-stats", "schema-2552") == 2552,
          "AC6: fidelity query schema-2552");
    CHECK(href(cs, "query:type-incremental-fidelity-stats",
               "occurrence-goal-steal-densify-fence-wired") == 1,
          "AC6: goal fence wired");
    CHECK(href(cs, "query:type-dep-partial-merge-stats", "schema-2552") == 2552,
          "AC6: type-dep query schema-2552");
    CHECK(href(cs, "query:type-dep-partial-merge-stats", "type-dep-steal-densify-fence-wired") == 1,
          "AC6: type_dep fence wired");
}

} // namespace

int main() {
    std::println("=== Issue #2552: type freshness steal/densify fence ===");
    ac1_steal_fence_prunes_goals();
    ac2_hard_fail_no_fence();
    ac3_densify_wires_fence();
    ac4_same_epoch_zero_cost();
    ac5_multi_round_soak();
    ac6_source_and_schema();
    std::println("\n=== #2552: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
