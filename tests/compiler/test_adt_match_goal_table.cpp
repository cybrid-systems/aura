// @category: unit
// @reason: Issue #2564 — ADT match exhaustiveness goal table + delta
//          reverify roots for Soft delta fidelity. Issue #3005 extends
//          the same suite: dirty-cone seed + Production no Dynamic slide.
//          Issue #3045: under-mark cone-force on variant add / arm delete.
//
//   AC1: note goals + invalidate by ADT → reverify roots; Soft recheck path
//   AC2: no ADT/match mutation → zero invalidate / reverify-root counters
//   AC3: table size capped (AURA_ADT_GOAL_TABLE_CAP / default 256)
//   AC4: additive schema-2564 + source-cite
//   AC5: #2223/#2264 hard-gate + match_sites_present remain authoritative

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.dirty_propagation;
import aura.compiler.value;
import aura.core.type;

namespace {

using aura::compiler::AdtMatchGoal;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::ConstraintSystem;
using aura::compiler::kAdtExhaustCommitRecheckIssue;
using aura::compiler::kAdtExhaustCompleteSeedIssue;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;
using aura::test::g_failed;
using aura::test::g_passed;

struct UnitCs {
    TypeRegistry reg;
    ConstraintSystem cs;
    CompilerMetrics m;
    UnitCs()
        : cs(reg) {
        cs.set_metrics(&m);
    }
};

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

// ── AC1: note + invalidate → reverify roots ──
static void ac1_note_invalidate_reverify() {
    std::println("\n--- #2564 AC1: note goals + invalidate → reverify roots ---");
    UnitCs u;
    u.cs.set_current_epoch(1);
    u.cs.note_adt_match_goal(/*match_node=*/10, /*adt_type_id=*/42, /*hash=*/0xabc);
    u.cs.note_adt_match_goal(/*match_node=*/11, /*adt_type_id=*/42, /*hash=*/0xdef);
    u.cs.note_adt_match_goal(/*match_node=*/12, /*adt_type_id=*/99, /*hash=*/1);
    CHECK(u.cs.adt_match_goals_size() == 3, "AC1: three goals noted");
    CHECK(u.m.adt_goal_note_total.load() == 3, "AC1: note_total == 3");

    // Upsert same match_node
    u.cs.note_adt_match_goal(10, 42, 0x111);
    CHECK(u.cs.adt_match_goals_size() == 3, "AC1: upsert does not grow table");
    CHECK(u.cs.adt_match_goals_for_test()[0].covered_variants_hash == 0x111 ||
              u.cs.adt_match_goals_for_test()[1].covered_variants_hash == 0x111 ||
              u.cs.adt_match_goals_for_test()[2].covered_variants_hash == 0x111,
          "AC1: upsert updates hash");

    const auto n = u.cs.invalidate_adt_goals_for(42);
    CHECK(n == 2, "AC1: invalidate drops 2 goals for adt 42");
    CHECK(u.cs.adt_match_goals_size() == 1, "AC1: one goal remains (adt 99)");
    CHECK(u.cs.adt_reverify_roots_size() == 2, "AC1: two reverify roots");

    auto roots = u.cs.drain_adt_reverify_roots();
    CHECK(roots.size() == 2, "AC1: drain returns 2 roots");
    CHECK(u.cs.adt_reverify_roots_size() == 0, "AC1: drain clears roots");

    // Source wiring
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("note_adt_match_goal") != std::string::npos, "AC1: note wired in impl");
    CHECK(tci.find("invalidate_adt_goals_for") != std::string::npos ||
              tci.find("g_adt_reverify_pending_tls") != std::string::npos,
          "AC1: invalidate / pending reverify path");
    CHECK(tci.find("drain_adt_reverify_roots") != std::string::npos, "AC1: drain in partial path");
    CHECK(tci.find("absorb_pending_adt_reverify_roots") != std::string::npos, "AC1: absorb TLS");
}

// ── AC2: empty → zero ──
static void ac2_zero_work() {
    std::println("\n--- #2564 AC2: no goals → zero invalidate ---");
    UnitCs u;
    CHECK(u.cs.adt_match_goals_size() == 0, "AC2: empty table");
    CHECK(u.cs.invalidate_adt_goals_for(1) == 0, "AC2: invalidate empty → 0");
    CHECK(u.cs.adt_reverify_roots_size() == 0, "AC2: no roots");
    CHECK(u.m.adt_goal_invalidate_total.load() == 0, "AC2: invalidate counter 0");
    CHECK(u.m.adt_reverify_root_total.load() == 0, "AC2: reverify counter 0");
    auto roots = u.cs.drain_adt_reverify_roots();
    CHECK(roots.empty(), "AC2: drain empty");
}

// ── AC3: cap ──
static void ac3_cap() {
    std::println("\n--- #2564 AC3: table size capped ---");
    const auto cap = ConstraintSystem::adt_goal_table_cap();
    CHECK(cap >= 1, "AC3: cap >= 1");
    CHECK(cap <= 1'000'000, "AC3: cap sane");

    UnitCs u;
    // Force small cap via env is process-static after first call — test drop path
    // by overflowing default if we fill past size... better test: fill many and
    // ensure size never exceeds cap.
    const auto n = std::min(cap + 5, cap + 5);
    for (std::size_t i = 1; i <= n; ++i)
        u.cs.note_adt_match_goal(static_cast<std::uint32_t>(i), 1, i);
    CHECK(u.cs.adt_match_goals_size() <= cap, "AC3: size <= cap");
    if (n > cap)
        CHECK(u.m.adt_goal_cap_drop_total.load() > 0, "AC3: cap drop when overflow");

    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("AURA_ADT_GOAL_TABLE_CAP") != std::string::npos, "AC3: env cap");
    CHECK(tci.find("adt_goal_cap_drop_total") != std::string::npos, "AC3: cap drop counter");
}

// ── AC4: schema ──
static void ac4_schema() {
    std::println("\n--- #2564 AC4: schema-2564 + source-cite ---");
    const auto tixx = read_file("src/compiler/type_checker.ixx");
    CHECK(tixx.find("AdtMatchGoal") != std::string::npos, "AC4: AdtMatchGoal struct");
    CHECK(tixx.find("note_adt_match_goal") != std::string::npos, "AC4: note API");
    CHECK(tixx.find("invalidate_adt_goals_for") != std::string::npos, "AC4: invalidate API");
    CHECK(tixx.find("#2564") != std::string::npos, "AC4: cites #2564");

    const auto om = read_file("src/compiler/observability_metrics.h");
    CHECK(om.find("adt_goal_invalidate_total") != std::string::npos, "AC4: metrics");
    CHECK(om.find("adt_reverify_root_total") != std::string::npos, "AC4: reverify metric");

    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    CHECK(q.find("schema-2564") != std::string::npos, "AC4: schema-2564");
    CHECK(q.find("adt-goal-table-size") != std::string::npos, "AC4: table-size key");
    CHECK(q.find("adt-goal-invalidate-total") != std::string::npos, "AC4: invalidate key");
    CHECK(q.find("adt-reverify-root-total") != std::string::npos, "AC4: reverify key");

    CompilerService cs;
    CHECK(href(cs, "schema-2564") == 2564, "AC4: live schema-2564");
    CHECK(href(cs, "adt-goal-table-wired") == 1, "AC4: wired");
    CHECK(href(cs, "adt-goal-invalidate-total") >= 0, "AC4: invalidate queryable");
    CHECK(href(cs, "adt-reverify-root-total") >= 0, "AC4: reverify queryable");
    CHECK(href(cs, "adt-goal-table-size") >= 0, "AC4: table size queryable");
}

// ── AC5: hard-gate retained ──
static void ac5_hard_gate_retained() {
    std::println("\n--- #2564 AC5: #2223/#2264 hard-gate retained ---");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(aud.find("adt_exhaustiveness_hard_gate_wired") != std::string::npos ||
              aud.find("adt_invariant_fail") != std::string::npos,
          "AC5: ADT invariant counters present");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(emb.find("match_sites") != std::string::npos ||
              emb.find("match_sites_present") != std::string::npos,
          "AC5: match_sites force audit retained");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("check_match_exhaustiveness") != std::string::npos,
          "AC5: exhaustiveness check retained");
    CHECK(tci.find("invalidate_match_exhaust_for_adt_type") != std::string::npos,
          "AC5: existing invalidate retained");
    // Goal path does not remove hard-gate
    CHECK(read_file("tests/compiler/test_adt_hard_gate_exhaustiveness.cpp").size() > 0 ||
              read_file("tests/compiler/test_adt_exhaustiveness_audit.cpp").size() > 0,
          "AC5: existing ADT hard-gate tests still present");
}

// ── Issue #3005: dirty cone + Production no Dynamic slide ──
// AC1: variant invalidate + pattern seed → reverify roots + dirty-type seed
// AC2: Production unproven / via_dynamic → reject path (no Dynamic slide)
// AC3: Soft observe only
// AC4: Quiet empty → zero seed / zero roots
// AC5: schema-3005 + #2564/#2288/#2219/#2939 lineage
// AC6: linter + no test_issue_3005 / no docs/design

static void ac3005_1_cone_seed() {
    std::println("\n--- #3005 AC1: invalidate + pattern seed → cone roots ---");
    UnitCs u;
    u.cs.set_current_epoch(1);
    u.cs.note_adt_match_goal(10, 42, 0xabc);
    u.cs.note_adt_match_goal(11, 42, 0xdef);
    CHECK(u.cs.invalidate_adt_goals_for(42) == 2, "3005 AC1: invalidate 2");
    CHECK(u.cs.adt_reverify_roots_size() == 2, "3005 AC1: two reverify roots");
    const std::vector<std::uint32_t> extra{11, 99};
    const auto seeded = u.cs.seed_adt_reverify_from_match_nodes(extra);
    CHECK(seeded == 1, "3005 AC1: pattern seed inserts new match only");
    CHECK(u.cs.adt_reverify_roots_size() == 3, "3005 AC1: 11+10+99");
    u.cs.note_adt_exhaust_dirty_type(42);
    CHECK(u.cs.pending_full_solve_roots_size() >= 1 || !u.cs.touched_roots().empty(),
          "3005 AC1: dirty-type seeds solve_delta touched/pending");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("seed_adt_reverify_from_match_nodes") != std::string::npos,
          "3005 AC1: pattern seed wired");
    CHECK(tci.find("note_adt_exhaust_dirty_type") != std::string::npos,
          "3005 AC1: solve_delta dirty-type seed");
    CHECK(tci.find("adt_exhaust_cone_seed_total") != std::string::npos,
          "3005 AC1: cone-seed counter");
    CHECK(tci.find("kAdtExhaustDirtyConeIssue") != std::string::npos ||
              tci.find("#3005") != std::string::npos,
          "3005 AC1: cites #3005");
}

static void ac3005_2_production_no_dynamic() {
    std::println("\n--- #3005 AC2: Production no Dynamic slide ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto ev = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(tci.find("via_dynamic") != std::string::npos, "3005 AC2: via_dynamic stamped");
    CHECK(tci.find("adt_exhaust_production_reject_total") != std::string::npos,
          "3005 AC2: production reject counter");
    CHECK(tci.find("adt_exhaust_dynamic_slide_prevented_total") != std::string::npos,
          "3005 AC2: Dynamic-slide prevented");
    CHECK(tci.find("last_type_export_authoritative_ = false") != std::string::npos,
          "3005 AC2: Production fail-closed authority");
    CHECK(ev.find("via_dynamic") != std::string::npos, "3005 AC2: hard-gate / audit via_dynamic");
    CHECK(ev.find("exhaustiveness unproven (Dynamic subject)") != std::string::npos,
          "3005 AC2: hard-gate Dynamic message");
    CHECK(read_file("src/compiler/type_checker.ixx").find("via_dynamic") != std::string::npos,
          "3005 AC2: result field");
}

static void ac3005_3_soft_observe() {
    std::println("\n--- #3005 AC3: Soft observe only ---");
    UnitCs u;
    u.cs.note_adt_match_goal(7, 1, 1);
    (void)u.cs.invalidate_adt_goals_for(1);
    CHECK(u.cs.adt_reverify_roots_size() == 1, "3005 AC3: Soft still seeds roots");
    CHECK(u.m.adt_exhaust_production_reject_total.load() == 0,
          "3005 AC3: no production reject on unit CS");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("adt_exhaust_soft_observe_total") != std::string::npos,
          "3005 AC3: Soft observe counter");
    CHECK(tci.find("production_defaults_active()") != std::string::npos,
          "3005 AC3: Production vs Soft branch");
}

static void ac3005_4_quiet_empty() {
    std::println("\n--- #3005 AC4: Quiet empty → zero ---");
    UnitCs u;
    const std::vector<std::uint32_t> empty;
    CHECK(u.cs.seed_adt_reverify_from_match_nodes(empty) == 0, "3005 AC4: empty seed 0");
    CHECK(u.cs.adt_reverify_roots_size() == 0, "3005 AC4: no roots");
    CHECK(u.cs.invalidate_adt_goals_for(99) == 0, "3005 AC4: invalidate empty 0");
    u.cs.note_adt_exhaust_dirty_type(0);
    CHECK(u.cs.pending_full_solve_roots_size() == 0, "3005 AC4: type 0 no pending");
    CHECK(u.m.adt_exhaust_cone_seed_total.load() == 0, "3005 AC4: cone seed quiet");
}

static void ac3005_5_schema_lineage() {
    std::println("\n--- #3005 AC5: schema-3005 + lineage ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    CHECK(q.find("schema-3005") != std::string::npos, "3005 AC5: schema-3005");
    CHECK(q.find("adt-exhaust-cone-seed-total") != std::string::npos, "3005 AC5: cone-seed key");
    CHECK(q.find("adt-exhaust-production-reject-total") != std::string::npos,
          "3005 AC5: reject key");
    CHECK(q.find("adt-exhaust-dynamic-slide-prevented-total") != std::string::npos,
          "3005 AC5: slide key");
    CHECK(q.find("schema-2564") != std::string::npos, "3005 AC5: lineage #2564");
    CHECK(read_file("src/compiler/type_checker.ixx").find("kAdtExhaustDirtyConeIssue = 3005") !=
              std::string::npos,
          "3005 AC5: issue stamp");
    CHECK(read_file("src/compiler/type_checker_impl.cpp").find("#2288") != std::string::npos,
          "3005 AC5: lineage #2288");
    CHECK(read_file("src/compiler/evaluator_typecheck.cpp").find("#2219") != std::string::npos,
          "3005 AC5: lineage #2219");
    CHECK(read_file("src/compiler/type_checker_impl.cpp").find("#2939") != std::string::npos ||
              read_file("src/compiler/type_checker.ixx").find("#2939") != std::string::npos,
          "3005 AC5: lineage #2939");
    CompilerService cs;
    CHECK(href(cs, "schema-3005") == 3005, "3005 AC5: live schema-3005");
    CHECK(href(cs, "issue-3005") == 3005, "3005 AC5: live issue-3005");
    CHECK(href(cs, "adt-exhaust-dirty-cone-wired") == 1, "3005 AC5: wired");
    CHECK(href(cs, "adt-exhaust-cone-seed-total") >= 0, "3005 AC5: cone-seed queryable");
    CHECK(href(cs, "schema-2564") == 2564, "3005 AC5: schema-2564 preserved");
}

// ── Issue #3045: under-mark cone-force ──
// AC1: Production + variant add / arm delete → site forced into cone
// AC2: Soft under-mark → counter only
// AC3: Quiet (no ADT touch) zero extra
// AC4: schema-3045
// AC5: source cites dirty_propagation + evaluator_typecheck + mutate_type_gate

static void ac3045_1_undermark_force_cone() {
    std::println("\n--- #3045 AC1: under-mark force → cone ---");
    UnitCs u;
    u.cs.set_current_epoch(1);
    u.cs.note_adt_match_goal(10, 42, 0xabc);
    const std::vector<std::uint32_t> arm_only{10};
    const auto n = u.cs.force_adt_exhaust_undermark_from_match_nodes(arm_only);
    CHECK(n == 1, "3045 AC1: force inserts match site");
    CHECK(u.cs.adt_reverify_roots_size() == 1, "3045 AC1: reverify root present");
    CHECK(u.m.adt_exhaust_undermark_force_total.load() == 1, "3045 AC1: force counter");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("force_adt_exhaust_undermark_into_cone") != std::string::npos,
          "3045 AC1: ancestor force wired");
    CHECK(tci.find("collect_adt_ancestors_from_dirty") != std::string::npos,
          "3045 AC1: ancestor walk");
    CHECK(tci.find("kAdtExhaustUndermarkConeIssue") != std::string::npos ||
              tci.find("#3045") != std::string::npos,
          "3045 AC1: cites #3045");
    const auto dp = read_file("src/compiler/dirty_propagation.ixx");
    CHECK(dp.find("force_adt_exhaust_sites_into_cone") != std::string::npos,
          "3045 AC1: dirty_propagation cone-force");
    CHECK(read_file("src/compiler/evaluator_typecheck.cpp").find("#3045") != std::string::npos,
          "3045 AC1: evaluator_typecheck");
    CHECK(read_file("src/compiler/mutate_type_gate.hh").find("#3045") != std::string::npos,
          "3045 AC1: mutate_type_gate Hard");
}

static void ac3045_2_soft_observe() {
    std::println("\n--- #3045 AC2: Soft under-mark observe only ---");
    UnitCs u;
    u.cs.note_adt_match_goal(7, 1, 1);
    const std::vector<std::uint32_t> arm{7};
    (void)u.cs.force_adt_exhaust_undermark_from_match_nodes(arm);
    CHECK(u.cs.adt_reverify_roots_size() == 1, "3045 AC2: Soft still seeds roots");
    CHECK(u.m.adt_exhaust_production_reject_total.load() == 0,
          "3045 AC2: no production reject on unit CS");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("adt_exhaust_soft_observe_total") != std::string::npos,
          "3045 AC2: Soft observe counter retained");
    CHECK(tci.find("production_defaults_active()") != std::string::npos,
          "3045 AC2: Production vs Soft branch retained");
}

static void ac3045_3_quiet() {
    std::println("\n--- #3045 AC3: Quiet no ADT → zero extra ---");
    UnitCs u;
    const std::vector<std::uint32_t> empty;
    CHECK(u.cs.force_adt_exhaust_undermark_from_match_nodes(empty) == 0, "3045 AC3: empty force 0");
    CHECK(u.cs.adt_reverify_roots_size() == 0, "3045 AC3: no roots");
    CHECK(u.m.adt_exhaust_undermark_force_total.load() == 0, "3045 AC3: force counter quiet");
    const std::vector<aura::compiler::dirty::NodeId> empty_sites;
    CHECK(aura::compiler::dirty::force_adt_exhaust_sites_into_cone(empty_sites) == 0,
          "3045 AC3: dirty_propagation empty 0");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("Quiet AC3") != std::string::npos ||
              tci.find("no ADT ancestor") != std::string::npos,
          "3045 AC3: Quiet path cited");
}

static void ac3045_4_schema() {
    std::println("\n--- #3045 AC4: schema-3045 ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    CHECK(q.find("schema-3045") != std::string::npos, "3045 AC4: schema-3045");
    CHECK(q.find("adt-exhaust-undermark-force-total") != std::string::npos,
          "3045 AC4: undermark key");
    CHECK(q.find("schema-3005") != std::string::npos, "3045 AC4: lineage #3005");
    CHECK(read_file("src/compiler/type_checker.ixx").find("kAdtExhaustUndermarkConeIssue = 3045") !=
              std::string::npos,
          "3045 AC4: issue stamp");
    CHECK(read_file("src/compiler/observability_metrics.h")
                  .find("adt_exhaust_undermark_force_total") != std::string::npos,
          "3045 AC4: metrics field");
    CompilerService cs;
    CHECK(href(cs, "schema-3045") == 3045, "3045 AC4: live schema-3045");
    CHECK(href(cs, "issue-3045") == 3045, "3045 AC4: live issue-3045");
    CHECK(href(cs, "adt-exhaust-undermark-cone-wired") == 1, "3045 AC4: wired");
    CHECK(href(cs, "adt-exhaust-undermark-force-total") >= 0, "3045 AC4: force queryable");
    CHECK(href(cs, "schema-3005") == 3005, "3045 AC4: schema-3005 preserved");
}

static void ac3045_5_source_cites() {
    std::println("\n--- #3045 AC5: source cites + no invent ---");
    const auto t = read_file("tests/compiler/test_adt_match_goal_table.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_adt_exhaust_undermark_cone_3045.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac3045_1_undermark_force_cone") != std::string::npos, "3045 AC5: AC1");
    CHECK(lint.find("#3045") != std::string::npos, "3045 AC5: linter present");
    CHECK(build.find("check_adt_exhaust_undermark_cone_3045") != std::string::npos,
          "3045 AC5: build.py gate");
    CHECK(build.find("cmd_adt_exhaust_undermark_cone_3045_coverage") != std::string::npos,
          "3045 AC5: build.py cmd");
    CHECK(read_file("tests/compiler/test_issue_3045.cpp").empty(),
          "3045 AC5: no test_issue_3045.cpp");
    CHECK(
        read_file("src/compiler/dirty_propagation.ixx").find("force_adt_exhaust_sites_into_cone") !=
            std::string::npos,
        "3045 AC5: dirty_propagation");
    CHECK(read_file("src/compiler/evaluator_typecheck.cpp").find("force_adt_exhaust_undermark") !=
              std::string::npos,
          "3045 AC5: evaluator_typecheck force");
    CHECK(read_file("src/compiler/mutate_type_gate.hh").find("under-mark cone-force") !=
              std::string::npos,
          "3045 AC5: mutate_type_gate");
}

// ── Issue #3083: complete seed after ADT / pattern mutate ──
// AC1: every match of a mutated ADT type is seeded (no silent under-seed)
// AC2: Production residual under-mark still TypeError + !authoritative
// AC3: Soft under-mark observe only
// AC4: empty types / no ADT → zero extra
// AC5: schema-3083 + #3045/#3005 lineage; extend this suite; linter

static void ac3083_1_complete_seed_incomplete_list() {
    std::println("\n--- #3083 AC1: incomplete seed completes sibling matches ---");
    CHECK(kAdtExhaustCompleteSeedIssue == 3083, "3083 AC1: issue stamp");
    UnitCs u;
    u.cs.set_current_epoch(1);
    u.cs.note_adt_match_goal(10, 42, 0xabc);
    u.cs.note_adt_match_goal(20, 42, 0xdef);
    u.cs.note_adt_match_goal(30, 99, 1);
    // Call site lists only match 10 (under-seed).
    const std::vector<std::uint32_t> incomplete{10};
    CHECK(u.cs.force_adt_exhaust_undermark_from_match_nodes(incomplete) == 1,
          "3083 AC1: listed force inserts 10");
    CHECK(u.cs.adt_reverify_roots_size() == 1, "3083 AC1: under-seeded before complete");
    const std::vector<std::uint32_t> types{42};
    const auto extra = u.cs.seed_adt_matches_for_dirty_types(types);
    CHECK(extra == 1, "3083 AC1: sibling 20 completed");
    CHECK(u.cs.adt_reverify_roots_size() == 2, "3083 AC1: 10+20 of type 42");
    CHECK(u.cs.pending_full_solve_roots_size() >= 1, "3083 AC1: note_adt_exhaust_dirty_type");
    CHECK(u.m.adt_exhaust_complete_seed_total.load() == 1, "3083 AC1: complete-seed counter");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("seed_adt_matches_for_dirty_types") != std::string::npos,
          "3083 AC1: CS complete seed");
    CHECK(tci.find("collect_match_sites_for_adt_types") != std::string::npos,
          "3083 AC1: FlatAST complete collect");
    CHECK(tci.find("Issue #3083") != std::string::npos, "3083 AC1: force cites #3083");
    CHECK(tci.find("note_adt_exhaust_dirty_type") != std::string::npos,
          "3083 AC1: dirty-type seed retained");
}

static void ac3083_2_production_reject_retained() {
    std::println("\n--- #3083 AC2: Production residual under-mark still rejects ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto ev = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(tci.find("last_type_export_authoritative_ = false") != std::string::npos,
          "3083 AC2: Production authority cleared");
    CHECK(tci.find("adt_exhaust_production_reject_total") != std::string::npos,
          "3083 AC2: Production reject counter");
    CHECK(tci.find("adt_exhaust_dynamic_slide_prevented_total") != std::string::npos,
          "3083 AC2: no Dynamic slide");
    CHECK(tci.find("ErrorKind::TypeError") != std::string::npos, "3083 AC2: TypeError");
    CHECK(ev.find("Issue #3083") != std::string::npos, "3083 AC2: Hard walk cites complete seed");
    CHECK(ev.find("mutate_type_gate") != std::string::npos, "3083 AC2: Hard gate retained");
}

static void ac3083_3_soft_observe() {
    std::println("\n--- #3083 AC3: Soft under-mark observe only ---");
    UnitCs u;
    u.cs.note_adt_match_goal(7, 1, 1);
    u.cs.note_adt_match_goal(8, 1, 2);
    const std::vector<std::uint32_t> types{1};
    CHECK(u.cs.seed_adt_matches_for_dirty_types(types) == 2, "3083 AC3: Soft still completes seed");
    CHECK(u.m.adt_exhaust_production_reject_total.load() == 0,
          "3083 AC3: no production reject on unit CS");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("adt_exhaust_soft_observe_total") != std::string::npos,
          "3083 AC3: Soft observe retained");
    CHECK(tci.find("production_defaults_active()") != std::string::npos,
          "3083 AC3: Production vs Soft branch retained");
}

static void ac3083_4_quiet_empty() {
    std::println("\n--- #3083 AC4: empty types / no ADT → zero extra ---");
    UnitCs u;
    const std::vector<std::uint32_t> empty;
    CHECK(u.cs.seed_adt_matches_for_dirty_types(empty) == 0, "3083 AC4: empty types 0");
    CHECK(u.cs.adt_reverify_roots_size() == 0, "3083 AC4: no roots");
    const std::vector<std::uint32_t> zero{0};
    CHECK(u.cs.seed_adt_matches_for_dirty_types(zero) == 0, "3083 AC4: type 0 ignored");
    CHECK(u.m.adt_exhaust_complete_seed_total.load() == 0, "3083 AC4: counter quiet");
    CHECK(u.cs.pending_full_solve_roots_size() == 0, "3083 AC4: no pending");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("Empty types") != std::string::npos ||
              tci.find("types.empty()") != std::string::npos,
          "3083 AC4: Quiet path cited");
}

static void ac3083_5_schema_and_linter() {
    std::println("\n--- #3083 AC5: schema + linter + no invent ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    CHECK(q.find("schema-3083") != std::string::npos, "3083 AC5: schema-3083");
    CHECK(q.find("adt-exhaust-complete-seed-total") != std::string::npos,
          "3083 AC5: complete-seed key");
    CHECK(q.find("schema-3045") != std::string::npos, "3083 AC5: lineage #3045");
    CHECK(q.find("schema-3005") != std::string::npos, "3083 AC5: lineage #3005");
    CHECK(read_file("src/compiler/type_checker.ixx").find("kAdtExhaustCompleteSeedIssue = 3083") !=
              std::string::npos,
          "3083 AC5: issue stamp");
    CHECK(
        read_file("src/compiler/observability_metrics.h").find("adt_exhaust_complete_seed_total") !=
            std::string::npos,
        "3083 AC5: metrics field");
    CompilerService cs;
    CHECK(href(cs, "schema-3083") == 3083, "3083 AC5: live schema-3083");
    CHECK(href(cs, "issue-3083") == 3083, "3083 AC5: live issue-3083");
    CHECK(href(cs, "adt-exhaust-complete-seed-wired") == 1, "3083 AC5: wired");
    CHECK(href(cs, "adt-exhaust-complete-seed-total") >= 0, "3083 AC5: complete-seed queryable");
    CHECK(href(cs, "schema-3045") == 3045, "3083 AC5: schema-3045 preserved");
    CHECK(href(cs, "schema-3005") == 3005, "3083 AC5: schema-3005 preserved");
    const auto t = read_file("tests/compiler/test_adt_match_goal_table.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_adt_exhaust_complete_seed_3083.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac3083_1_complete_seed_incomplete_list") != std::string::npos, "3083 AC5: AC1");
    CHECK(t.find("ac3083_2_production_reject_retained") != std::string::npos, "3083 AC5: AC2");
    CHECK(t.find("ac3083_3_soft_observe") != std::string::npos, "3083 AC5: AC3");
    CHECK(t.find("ac3083_4_quiet_empty") != std::string::npos, "3083 AC5: AC4");
    CHECK(!lint.empty() && lint.find("Issue #3083") != std::string::npos, "3083 AC5: linter");
    CHECK(build.find("check_adt_exhaust_complete_seed_3083") != std::string::npos,
          "3083 AC5: build.py");
    CHECK(read_file("tests/compiler/test_issue_3083.cpp").empty(),
          "3083 AC5: no invent test_issue_3083");
    CHECK(read_file("docs/design/3083-adt-exhaust-complete-seed.md").empty(),
          "3083 AC5: no docs/design/");
}

// ── Issue #3236: match+arms cone + Production recheck before proof ──
static void ac3236_1_match_and_arms_into_cone() {
    std::println("\n--- #3236 AC1: match node + arms enter cone ---");
    CHECK(kAdtExhaustCommitRecheckIssue == 3236, "3236 AC1: issue stamp");
    const std::vector<aura::compiler::dirty::NodeId> match_and_arms{10, 11, 12};
    CHECK(aura::compiler::dirty::force_adt_exhaust_sites_into_cone(match_and_arms) == 3,
          "3236 AC1: match+2 arms forced");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("Issue #3236") != std::string::npos, "3236 AC1: impl cite");
    CHECK(tci.find("flat.children(nid)") != std::string::npos, "3236 AC1: arm children");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(etc.find("Issue #3236") != std::string::npos, "3236 AC1: commit recheck");
    CHECK(etc.find("force_reason=*/1") != std::string::npos ||
              etc.find("force_reason = 1") != std::string::npos ||
              etc.find("/*force_reason=*/1") != std::string::npos,
          "3236 AC1: reused solve force_reason");
}

static void ac3236_2_soft_quiet() {
    std::println("\n--- #3236 AC2: Soft observe; quiet empty ---");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(etc.find("adt_exhaust_soft_observe_total") != std::string::npos,
          "3236 AC2: Soft observe");
    CHECK(etc.find("goals || cone") != std::string::npos, "3236 AC2: quiet skip when no ADT");
    const std::vector<aura::compiler::dirty::NodeId> empty;
    CHECK(aura::compiler::dirty::force_adt_exhaust_sites_into_cone(empty) == 0,
          "3236 AC2: empty sites 0 extra");
}

static void ac3236_3_lineage() {
    std::println("\n--- #3236 AC3: no regression #3045/#3083 ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("force_adt_exhaust_undermark_into_cone") != std::string::npos,
          "3236 AC3: #3045 force retained");
    CHECK(tci.find("seed_adt_matches_for_dirty_types") != std::string::npos,
          "3236 AC3: #3083 complete seed retained");
    CHECK(read_file("src/compiler/dirty_propagation.ixx")
                  .find("force_residual_castop_undermark_into_cone") != std::string::npos,
          "3236 AC3: #3228 residual CastOp retained");
}

static void ac3236_4_source_linter() {
    std::println("\n--- #3236 AC4: source-cite + linter + no invent ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto build = read_file("build.py");
    CHECK(ixx.find("kAdtExhaustCommitRecheckIssue = 3236") != std::string::npos, "3236 AC4: stamp");
    CHECK(build.find("check_adt_exhaust_commit_recheck_3236") != std::string::npos,
          "3236 AC4: build.py");
    CHECK(read_file("tests/compiler/test_issue_3236.cpp").empty(), "3236 AC4: no invent");
    CHECK(read_file("docs/design/3236-adt-exhaust-commit.md").empty(), "3236 AC4: no docs/design");
}

static void ac3005_6_linter_no_design() {
    std::println("\n--- #3005 AC6: linter + no invent / no design ---");
    const auto t = read_file("tests/compiler/test_adt_match_goal_table.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_adt_exhaust_dirty_cone_3005.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac3005_1_cone_seed") != std::string::npos, "3005 AC6: AC1");
    CHECK(t.find("ac3005_2_production_no_dynamic") != std::string::npos, "3005 AC6: AC2");
    CHECK(t.find("ac3005_3_soft_observe") != std::string::npos, "3005 AC6: AC3");
    CHECK(t.find("ac3005_4_quiet_empty") != std::string::npos, "3005 AC6: AC4");
    CHECK(t.find("ac3005_5_schema_lineage") != std::string::npos, "3005 AC6: AC5");
    CHECK(lint.find("check_adt_exhaust_dirty_cone_3005") != std::string::npos ||
              lint.find("#3005") != std::string::npos,
          "3005 AC6: linter present");
    CHECK(build.find("check_adt_exhaust_dirty_cone_3005") != std::string::npos,
          "3005 AC6: build.py gate");
    CHECK(build.find("cmd_adt_exhaust_dirty_cone_3005_coverage") != std::string::npos,
          "3005 AC6: build.py cmd");
    CHECK(read_file("tests/compiler/test_issue_3005.cpp").empty(),
          "3005 AC6: no test_issue_3005.cpp");
}

} // namespace

int run_test_adt_match_goal_table() {
    std::println("=== Issue #2564 / #3005 / #3045: ADT match goal table + dirty cone ===");
    ac1_note_invalidate_reverify();
    ac2_zero_work();
    ac3_cap();
    ac4_schema();
    ac5_hard_gate_retained();
    ac3005_1_cone_seed();
    ac3005_2_production_no_dynamic();
    ac3005_3_soft_observe();
    ac3005_4_quiet_empty();
    ac3005_5_schema_lineage();
    ac3005_6_linter_no_design();
    ac3045_1_undermark_force_cone();
    ac3045_2_soft_observe();
    ac3045_3_quiet();
    ac3045_4_schema();
    ac3045_5_source_cites();
    ac3083_1_complete_seed_incomplete_list();
    ac3083_2_production_reject_retained();
    ac3083_3_soft_observe();
    ac3083_4_quiet_empty();
    ac3083_5_schema_and_linter();
    ac3236_1_match_and_arms_into_cone();
    ac3236_2_soft_quiet();
    ac3236_3_lineage();
    ac3236_4_source_linter();
    std::println("\n=== #2564/#3005/#3045/#3083/#3236: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_adt_match_goal_table();
}
#endif
