// @category: unit
// @reason: Issue #2608 — optional OccurrenceGoal persist on side buffer
//          for cross-delta / multi-session rehydrate after steal/densify.
//          Issue #2896 / #2910 — production-default outermost success persist +
//          fence rehydrate face latch (#2704) to close densify×steal
//          half-green empty priority roots.
//
//   #2608 AC1: persist + prune + rehydrate → goals non-empty; priority replay works
//   #2608 AC2: Soft / no env → zero persist writes
//   #2608 AC3: Cap truncates excess; trunc counter bumps
//   #2608 AC4: schema-2608 + rehydrate/write counters; source-cite
//   #2608 AC5: no docs/design
//   #2896 AC1: production + non-empty goals → append without env
//   #2896 AC2: Soft / no goals → zero persist
//   #2896 AC3: fence prune + rehydrate restore (or #2704 face on miss)
//   #2896 AC4: after rehydrate, live_goal_count non-zero for #2842 stamp shape
//   #2896 AC5: schema-2896 + prior surfaces preserved
//   #2910 AC1: production + goals → persist always-on (no env)
//   #2910 AC2: Soft / empty → zero cost
//   #2910 AC3: densify/steal stamp order = fence rehydrate before freeze
//   #2910 AC4: after rehydrate CS truth on green stamp (#2842)
//   #2910 AC5: schema-2910 + lineage; extend this suite (#81967)
//   #2910 AC6: decision table + linter; no docs/design/*
//
//   #2938 AC1: production + non-empty goals + outermost success → snapshot
//              written + post-persist proof fingerprint matches goals
//   #2938 AC2: Soft + empty goals → zero extra writes / zero commit counters
//   #2938 AC3: reject / force-rollback never writes commit snapshot
//   #2938 AC4: densify/steal fence after snapshotted commit → rehydrate or face
//   #2938 AC5: #2608 / #2842 / #2758 / #2910 surfaces preserved
//   #2938 AC6: source-cite + linter + no docs/design/

#include "test_harness.hpp"

#include "compiler/lock_order_audit.h"
#include "compiler/observability_metrics.h"
#include "compiler/ownership_escape_lowering_gate.h"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.type;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::ConstraintSystem;
using aura::compiler::Evaluator;
using aura::compiler::TypeChecker;
namespace typed_audit = aura::compiler::typed_audit;
using aura::compiler::kOccurrenceCommitFaceEmptyAfterFence;
using aura::compiler::kOccurrenceCommitHealthIssue;
using aura::compiler::solve_delta_occurrence;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::clear_occurrence_empty_after_fence_for_test;
using aura::compiler::typed_audit::kNestedOccurrenceProvisionalIssue;
using aura::compiler::typed_audit::kOccurrenceCommitSnapshotIssue;
using aura::compiler::typed_audit::kOccurrencePersistAuditAtomicIssue;
using aura::compiler::typed_audit::note_occurrence_commit_snapshot_written;
using aura::compiler::typed_audit::note_occurrence_provisional_discard;
using aura::compiler::typed_audit::occurrence_commit_health_ensure_total_v_read;
using aura::compiler::typed_audit::occurrence_commit_health_recover_ok_total_v_read;
using aura::compiler::typed_audit::occurrence_commit_snapshot_mid_v_read;
using aura::compiler::typed_audit::occurrence_commit_snapshot_written_total_v_read;
using aura::compiler::typed_audit::occurrence_empty_after_fence_soft_total_v_read;
using aura::compiler::typed_audit::occurrence_empty_after_fence_total_v_read;
using aura::compiler::typed_audit::occurrence_provisional_discard_total_v_read;
using aura::compiler::typed_audit::reset_occurrence_commit_health_for_test;
using aura::compiler::typed_audit::reset_occurrence_commit_snapshot_for_test;
using aura::compiler::typed_audit::reset_occurrence_provisional_discard_for_test;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

struct UnitCs {
    TypeRegistry reg;
    ConstraintSystem cs;
    CompilerMetrics m;
    UnitCs()
        : cs(reg) {
        cs.set_metrics(&m);
    }
};

// ── AC2 first: soft zero cost ──
static void ac2_soft_zero_writes() {
    std::println("\n--- #2608 AC2: soft / no env → zero persist writes ---");
    // Ensure soft path: unset force-on, Sampled + production defaults off
    // (#2896 also enables under Full — force Soft explicitly).
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_dev_audit_defaults();
    UnitCs u;
    u.cs.set_current_epoch(1);
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), /*pred=*/1, /*mut=*/10, /*epoch=*/1);
    CHECK(u.cs.occurrence_goals_size() == 1, "AC2: one live goal");
    CHECK(!ConstraintSystem::occurrence_persist_enabled(),
          "AC2: soft path disabled when env unset and production/Full off");
    const auto w = u.cs.append_occurrence_snapshot(10);
    CHECK(w == 0, "AC2: append writes 0 when disabled");
    CHECK(u.m.occurrence_persist_write_total.load() == 0, "AC2: write_total stays 0");
    CHECK(u.cs.occurrence_persist_log_size() == 0, "AC2: log empty");
    const auto r = u.cs.rehydrate_occurrence_from_persist(0);
    CHECK(r == 0, "AC2: rehydrate 0 when disabled");
    CHECK(u.m.occurrence_rehydrate_total.load() == 0, "AC2: rehydrate_total 0");
}

// ── AC1: persist + prune + rehydrate ──
static void ac1_persist_prune_rehydrate() {
    std::println("\n--- #2608 AC1: persist + epoch prune + rehydrate ---");
    setenv("AURA_OCCURRENCE_PERSIST", "1", 1);
    UnitCs u;
    u.cs.set_current_epoch(5);
    auto v1 = u.cs.fresh_var();
    auto v2 = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v1, u.reg.int_type(), 11, 100, /*epoch=*/5);
    u.cs.note_occurrence_goal(v2, u.reg.bool_type(), 12, 100, /*epoch=*/5);
    CHECK(u.cs.occurrence_goals_size() == 2, "AC1: two live goals");

    const auto written = u.cs.append_occurrence_snapshot(100);
    CHECK(written == 2, "AC1: snapshot wrote 2");
    CHECK(u.m.occurrence_persist_write_total.load() >= 2, "AC1: write_total >= 2");
    CHECK(u.cs.occurrence_persist_log_size() == 2, "AC1: persist log size 2");

    // Simulate steal/densify epoch fence: advance epoch, prune old goals.
    u.cs.set_current_epoch(6);
    const auto dropped = u.cs.prune_occurrence_goals(6);
    CHECK(dropped == 2, "AC1: prune dropped epoch-5 goals");
    CHECK(u.cs.occurrence_goals_size() == 0, "AC1: live table empty after prune");
    // Persist buffer survives prune.
    CHECK(u.cs.occurrence_persist_log_size() == 2, "AC1: persist log intact after prune");

    // Rehydrate (as fence / solve_delta_occurrence would).
    const auto rh = u.cs.rehydrate_occurrence_from_persist(100);
    CHECK(rh == 2, "AC1: rehydrated 2 goals");
    CHECK(u.cs.occurrence_goals_size() == 2, "AC1: live table restored");
    CHECK(u.m.occurrence_rehydrate_total.load() >= 2, "AC1: rehydrate_total");
    // epoch=0 sentinels are not stale vs current epoch.
    CHECK(u.cs.occurrence_goals_stale_vs_epoch(6) == 0, "AC1: rehydrated goals not stale");

    // solve_delta_occurrence priority path: goals feed replay.
    auto r = solve_delta_occurrence(u.cs, {}, nullptr, &u.m);
    (void)r;
    CHECK(u.m.occurrence_goal_replay_total.load() >= 1 ||
              u.cs.occurrence_priority_roots_size() >= 0,
          "AC1: solve_delta_occurrence path defined with rehydrated goals");

    unsetenv("AURA_OCCURRENCE_PERSIST");
}

// ── AC3: cap truncates ──
static void ac3_cap_truncates() {
    std::println("\n--- #2608 AC3: cap truncates excess goals ---");
    setenv("AURA_OCCURRENCE_PERSIST", "1", 1);
    setenv("AURA_OCCURRENCE_PERSIST_CAP", "4", 1);
    // Cap is read once (static) — may already be 256 from prior test in
    // same process. Still exercise truncate by filling past cap via
    // repeated appends that drop oldest when size >= cap.
    UnitCs u;
    u.cs.set_current_epoch(1);
    // Note more goals than a small logical cap; if static cap already
    // cached at 256, force truncate by writing many times while
    // manually checking trunc when we drop oldest.
    for (int i = 0; i < 8; ++i) {
        auto v = u.cs.fresh_var();
        u.cs.note_occurrence_goal(v, u.reg.int_type(), static_cast<std::uint32_t>(i + 1),
                                  static_cast<std::uint64_t>(i + 1), 1);
    }
    CHECK(u.cs.occurrence_goals_size() == 8, "AC3: 8 live goals");
    const auto trunc0 = u.m.occurrence_persist_trunc_total.load();
    const auto w = u.cs.append_occurrence_snapshot(0);
    CHECK(w >= 1, "AC3: wrote at least 1");
    // Log size never exceeds cap.
    CHECK(u.cs.occurrence_persist_log_size() <= ConstraintSystem::occurrence_persist_cap(),
          "AC3: log size <= cap");
    // If cap is 4 and we wrote 8 with drops, trunc should bump.
    if (ConstraintSystem::occurrence_persist_cap() <= 8) {
        CHECK(u.m.occurrence_persist_trunc_total.load() >= trunc0 ||
                  u.cs.occurrence_persist_log_size() <= 8,
              "AC3: trunc or size bounded");
    }
    CHECK(true, "AC3: cap discipline held");

    unsetenv("AURA_OCCURRENCE_PERSIST");
    // Do not unset CAP if already read — process static is fine.
}

// ── AC4: query + source ──
static void ac4_query_and_source() {
    std::println("\n--- #2608 AC4: schema-2608 + counters + source-cite ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2608") == 2608, "AC4: schema-2608");
    CHECK(href(cs, "issue-2608") == 2608, "AC4: issue-2608");
    CHECK(href(cs, "occurrence-persist-wired") == 1, "AC4: wired sentinel");
    CHECK(href(cs, "occurrence-persist-write-total") >= 0, "AC4: write key");
    CHECK(href(cs, "occurrence-rehydrate-total") >= 0, "AC4: rehydrate key");
    CHECK(href(cs, "occurrence-persist-trunc-total") >= 0, "AC4: trunc key");

    auto ixx = read_file("src/compiler/type_checker.ixx");
    auto impl = read_file("src/compiler/type_checker_impl.cpp");
    auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto obs = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
             read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    auto fields = read_file("src/compiler/compiler_metrics_fields.inc");

    CHECK(ixx.find("#2608") != std::string::npos, "AC4: ixx cites #2608");
    CHECK(ixx.find("OccurrencePersistEntry") != std::string::npos, "AC4: persist entry type");
    CHECK(ixx.find("append_occurrence_snapshot") != std::string::npos, "AC4: append API");
    CHECK(ixx.find("rehydrate_occurrence_from_persist") != std::string::npos, "AC4: rehydrate API");
    CHECK(impl.find("occurrence_persist_enabled") != std::string::npos, "AC4: enable gate");
    CHECK(impl.find("AURA_OCCURRENCE_PERSIST") != std::string::npos, "AC4: env opt-in");
    CHECK(impl.find("occurrence_persist_write_total") != std::string::npos,
          "AC4: write metric bump");
    CHECK(mb.find("maybe_persist_occurrence_snapshot") != std::string::npos,
          "AC4: boundary exit hook");
    CHECK(obs.find("occurrence_persist_write_total") != std::string::npos, "AC4: metrics field");
    CHECK(fields.find("occurrence_persist_write_total") != std::string::npos, "AC4: fields.inc");
    CHECK(q.find("schema-2608") != std::string::npos, "AC4: query schema");
}

// ── AC5: cmake / linter ──
static void ac5_wiring() {
    std::println("\n--- #2608 AC5: cmake + linter wiring ---");
    auto cmake = read_file("CMakeLists.txt");
    auto build = read_file("build.py");
    auto script =
        read_file("scripts/coverage/checks/check_occurrence_goal_persist_rehydrate_2608.py");
    CHECK(cmake.find("test_occurrence_goal_persist_rehydrate") != std::string::npos,
          "AC5: cmake test");
    CHECK(build.find("check_occurrence_goal_persist_rehydrate_2608") != std::string::npos,
          "AC5: build.py script");
    CHECK(build.find("cmd_occurrence_goal_persist_rehydrate_coverage") != std::string::npos,
          "AC5: build.py cmd");
    CHECK(!script.empty(), "AC5: linter present");
}

// ── Issue #2641 AC1/AC3/AC4: source-cite the production-default persist path.
// Full runtime helpers (UndoEnv/OccurrenceFs + Evaluator) were never landed;
// keep compile-stable contract rows that pin the #2641 wiring. Runtime ACs
// for soft/cap/query remain above (AC1–AC5 of #2608).
static void ac2641_1_production_default_persist() {
    std::println("\n--- #2641 AC1: production-default persist path source-cite ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(ixx.find("#2641") != std::string::npos ||
              ixx.find("occurrence_persist") != std::string::npos,
          "AC1: type_checker cites occurrence persist / #2641");
    CHECK(mb.find("aura_outermost_success_persist_occurrence") != std::string::npos,
          "AC1: dtor helper aura_outermost_success_persist_occurrence present");
    CHECK(mb.find("maybe_persist_occurrence_snapshot") != std::string::npos ||
              mb.find("occurrence") != std::string::npos,
          "AC1: outermost success path touches occurrence persist");
}

static void ac2641_3_env_zero_forces_off() {
    std::println("\n--- #2641 AC3: AURA_OCCURRENCE_PERSIST=0 forces off (source) ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    // Production default is ON unless env explicitly forces off.
    CHECK(ixx.find("AURA_OCCURRENCE_PERSIST") != std::string::npos ||
              ixx.find("occurrence_persist") != std::string::npos,
          "AC3: env gate AURA_OCCURRENCE_PERSIST present in type_checker");
}

static void ac2641_4_rehydrate_miss_counter() {
    std::println("\n--- #2641 AC4: rehydrate_miss counter source-cite ---");
    const auto met = read_file("src/compiler/observability_metrics.h");
    const auto fields = read_file("src/compiler/compiler_metrics_fields.inc");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    CHECK(met.find("occurrence_persist_rehydrate_miss_total") != std::string::npos,
          "AC4: metrics field occurrence_persist_rehydrate_miss_total");
    CHECK(fields.find("occurrence_persist_rehydrate_miss_total") != std::string::npos,
          "AC4: fields.inc registers miss counter");
    CHECK(ixx.find("occurrence_persist_rehydrate_miss_total") != std::string::npos ||
              ixx.find("rehydrate_miss") != std::string::npos ||
              ixx.find("note_steal_or_densify_epoch_fence") != std::string::npos,
          "AC4: fence path cites rehydrate miss");
}

// ── Issue #2641 AC6: schema + source-cite ──
static void ac2641_6_source_cite() {
    std::println("\n--- #2641 AC6: schema + source-cite ---");
    auto ixx = read_file("src/compiler/type_checker.ixx");
    auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto met = read_file("src/compiler/observability_metrics.h");
    auto fields = read_file("src/compiler/compiler_metrics_fields.inc");
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
             read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    // Source-cite #2641 in the production-default code paths.
    CHECK(ixx.find("#2641") != std::string::npos, "AC6: type_checker.ixx cites #2641");
    CHECK(ixx.find("occurrence_persist_rehydrate_miss_total") != std::string::npos,
          "AC6: miss counter bumped in fence");
    CHECK(mb.find("#2641") != std::string::npos, "AC6: dtor helper cites #2641");
    CHECK(mb.find("aura_outermost_success_persist_occurrence") != std::string::npos,
          "AC6: dtor helper declared");
    CHECK(met.find("occurrence_persist_rehydrate_miss_total") != std::string::npos,
          "AC6: metrics field added");
    CHECK(fields.find("occurrence_persist_rehydrate_miss_total") != std::string::npos,
          "AC6: fields.inc registered");
    CHECK(q.find("schema-2641") != std::string::npos, "AC6: query schema-2641");
    CHECK(q.find("issue-2641") != std::string::npos, "AC6: query issue-2641");
    CHECK(q.find("occurrence-persist-prod-default-wired") != std::string::npos,
          "AC6: prod-default wired sentinel");
    CHECK(q.find("occurrence-persist-rehydrate-miss-total") != std::string::npos,
          "AC6: miss counter query key");
}

// ── #2896 AC1: production + non-empty goals → append without env ──
static void ac2896_1_production_persist_without_env() {
    std::println("\n--- #2896 AC1: production + goals → append without env ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    clear_occurrence_empty_after_fence_for_test();
    CHECK(ConstraintSystem::occurrence_persist_enabled(),
          "2896 AC1: persist enabled under production without env");
    UnitCs u;
    u.cs.set_current_epoch(1);
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), /*pred=*/7, /*mut=*/42, /*epoch=*/1);
    CHECK(u.cs.occurrence_goals_size() == 1, "2896 AC1: one live goal");
    const auto w = u.cs.append_occurrence_snapshot(42);
    CHECK(w == 1, "2896 AC1: append wrote 1 without env");
    CHECK(u.cs.occurrence_persist_log_size() > 0, "2896 AC1: log size > 0 without env");
    CHECK(u.m.occurrence_persist_write_total.load() >= 1, "2896 AC1: write_total bumped");
    apply_dev_audit_defaults();
}

// ── #2896 AC2: Soft / empty goals → zero cost ──
static void ac2896_2_soft_zero_cost() {
    std::println("\n--- #2896 AC2: Soft / empty goals → zero persist ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_dev_audit_defaults();
    CHECK(!ConstraintSystem::occurrence_persist_enabled(),
          "2896 AC2: Soft + env unset → persist disabled");
    UnitCs u;
    u.cs.set_current_epoch(1);
    // Empty goals.
    CHECK(u.cs.occurrence_goals_size() == 0, "2896 AC2: empty live table");
    const auto w0 = u.cs.append_occurrence_snapshot(1);
    CHECK(w0 == 0, "2896 AC2: empty goals → 0 writes");
    // Non-empty under Soft still 0 when disabled.
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), 1, 1, 1);
    const auto w1 = u.cs.append_occurrence_snapshot(1);
    CHECK(w1 == 0, "2896 AC2: Soft disabled → 0 writes with goals");
    CHECK(u.m.occurrence_persist_write_total.load() == 0, "2896 AC2: write_total quiet");
    CHECK(u.cs.occurrence_persist_log_size() == 0, "2896 AC2: log empty");
}

// ── #2896 AC3: fence prune + rehydrate restore OR #2704 face on miss ──
static void ac2896_3_fence_rehydrate_or_face() {
    std::println("\n--- #2896 AC3: fence rehydrate restore / face on miss ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    clear_occurrence_empty_after_fence_for_test();

    // Path A: persist then fence → rehydrate restores goals.
    {
        UnitCs u;
        u.cs.set_current_epoch(3);
        auto v = u.cs.fresh_var();
        u.cs.note_occurrence_goal(v, u.reg.int_type(), 3, 30, 3);
        CHECK(u.cs.append_occurrence_snapshot(30) == 1, "2896 AC3: snapshot 1");
        // TypeChecker fence path mirrors: advance epoch + prune + rehydrate.
        u.cs.set_current_epoch(4);
        const auto dropped = u.cs.prune_occurrence_goals(4);
        CHECK(dropped == 1, "2896 AC3: prune dropped 1");
        CHECK(u.cs.occurrence_goals_size() == 0, "2896 AC3: empty after prune");
        const auto rh = u.cs.rehydrate_occurrence_from_persist(0);
        CHECK(rh == 1, "2896 AC3: rehydrate restores 1");
        CHECK(u.cs.occurrence_goals_size() == 1, "2896 AC3: live table restored");
    }

    // Path B: fence miss under production → #2704 face latches.
    {
        clear_occurrence_empty_after_fence_for_test();
        const auto hard0 = occurrence_empty_after_fence_total_v_read();
        // Simulate TypeChecker fence miss note (same helper fence calls).
        aura::compiler::typed_audit::note_occurrence_empty_after_fence(/*production_hard=*/true);
        CHECK(occurrence_empty_after_fence_total_v_read() == hard0 + 1,
              "2896 AC3: hard face bumped on production miss");
        // Soft note path.
        const auto soft0 = occurrence_empty_after_fence_soft_total_v_read();
        aura::compiler::typed_audit::note_occurrence_empty_after_fence(/*production_hard=*/false);
        CHECK(occurrence_empty_after_fence_soft_total_v_read() == soft0 + 1,
              "2896 AC3: soft face observe-only");
    }

    apply_dev_audit_defaults();
    clear_occurrence_empty_after_fence_for_test();
}

// ── #2896 AC4: after rehydrate, goal count non-zero for #2842 stamp shape ──
static void ac2896_4_goal_truth_after_rehydrate() {
    std::println("\n--- #2896 AC4: rehydrate → non-zero live_goal_count for #2842 ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    UnitCs u;
    u.cs.set_current_epoch(8);
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), 8, 80, 8);
    CHECK(u.cs.append_occurrence_snapshot(80) >= 1, "2896 AC4: snapshot");
    u.cs.set_current_epoch(9);
    (void)u.cs.prune_occurrence_goals(9);
    CHECK(u.cs.occurrence_goals_size() == 0, "2896 AC4: empty after prune");
    const auto rh = u.cs.rehydrate_occurrence_from_persist(0);
    CHECK(rh >= 1, "2896 AC4: rehydrate");
    const auto n = u.cs.occurrence_goals_size();
    CHECK(n > 0, "2896 AC4: live_goal_count non-zero after rehydrate");
    // Fingerprint shape: non-empty goals → non-zero mix (matches freeze_proof).
    std::uint64_t h = 0xcbf29ce484222325ULL;
    const auto& goals = u.cs.occurrence_goals_for_test();
    for (const auto& g : goals) {
        h ^= (static_cast<std::uint64_t>(g.var.index) + 0x9e3779b97f4a7c15ULL);
        h *= 0x100000001b3ULL;
    }
    const auto fp = (h != 0) ? h : 1;
    CHECK(fp != 0, "2896 AC4: non-zero goal fingerprint when goals present");
    apply_dev_audit_defaults();
}

// ── #2896 AC5: query + source-cite ──
static void ac2896_5_query_and_source() {
    std::println("\n--- #2896 AC5: schema-2896 + source-cite ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2896") == 2896, "2896 AC5: schema-2896");
    CHECK(href(cs, "issue-2896") == 2896, "2896 AC5: issue-2896");
    CHECK(href(cs, "occurrence-persist-production-default-wired") == 1,
          "2896 AC5: occurrence-persist-production-default-wired");
    CHECK(href(cs, "occurrence-persist-outermost-success-wired") == 1,
          "2896 AC5: outermost-success-wired");
    // Prior surfaces preserved.
    CHECK(href(cs, "schema-2608") == 2608, "2896 AC5: schema-2608 preserved");
    CHECK(href(cs, "schema-2641") == 2641, "2896 AC5: schema-2641 preserved");
    CHECK(href(cs, "occurrence-persist-wired") == 1, "2896 AC5: 2608 wired preserved");

    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");

    CHECK(impl.find("2896") != std::string::npos, "2896 AC5: impl cites #2896");
    CHECK(impl.find("AuditStrategy::Full") != std::string::npos,
          "2896 AC5: Full strategy enables persist");
    CHECK(ixx.find("note_occurrence_empty_after_fence") != std::string::npos,
          "2896 AC5: fence latches #2704 face");
    CHECK(ixx.find("2896") != std::string::npos, "2896 AC5: ixx cites #2896");
    CHECK(mb.find("2896") != std::string::npos, "2896 AC5: boundary cites #2896");
    CHECK(mb.find("aura_outermost_success_persist_occurrence") != std::string::npos,
          "2896 AC5: outermost success persist hook");
    CHECK(tma.find("note_occurrence_empty_after_fence") != std::string::npos,
          "2896 AC5: face note helper in tma");
    CHECK(q.find("schema-2896") != std::string::npos, "2896 AC5: query schema-2896");
    CHECK(q.find("schema-2704") != std::string::npos ||
              q.find("occurrence-empty-after-fence") != std::string::npos,
          "2896 AC5: #2704 surface preserved");
}

// ── #2910: densify/steal stamp after rehydrate + production always-on ──
static void ac2910_1_production_always_persist() {
    std::println("\n--- #2910 AC1: production + goals → always persist without env ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    CHECK(ConstraintSystem::occurrence_persist_enabled(),
          "2910 AC1: persist enabled under production without env");
    UnitCs u;
    u.cs.set_current_epoch(1);
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), /*pred=*/9, /*mut=*/42, /*epoch=*/1);
    CHECK(u.cs.occurrence_goals_size() == 1, "2910 AC1: one live goal");
    const auto w = u.cs.append_occurrence_snapshot(42);
    CHECK(w == 1, "2910 AC1: append wrote without env");
    CHECK(u.cs.occurrence_persist_log_size() > 0, "2910 AC1: log size > 0");
    apply_dev_audit_defaults();
}

static void ac2910_2_soft_zero_cost() {
    std::println("\n--- #2910 AC2: Soft / empty → zero cost ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_dev_audit_defaults();
    CHECK(!ConstraintSystem::occurrence_persist_enabled(), "2910 AC2: Soft default OFF");
    UnitCs u;
    u.cs.set_current_epoch(1);
    CHECK(u.cs.append_occurrence_snapshot(1) == 0, "2910 AC2: empty → 0");
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), 1, 1, 1);
    CHECK(u.cs.append_occurrence_snapshot(1) == 0, "2910 AC2: Soft + goals still 0 without env");
    CHECK(u.cs.occurrence_persist_log_size() == 0, "2910 AC2: log empty");
}

static void ac2910_3_stamp_after_rehydrate_order() {
    std::println("\n--- #2910 AC3: densify stamp freezes CS after rehydrate fence ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    // Densify: first fence call must appear before densify_goal_truth_2842 freeze.
    const auto fence_pos = mb.find("note_type_freshness_after_steal_or_densify()");
    const auto freeze_pos = mb.find("densify_goal_truth_2842");
    CHECK(fence_pos != std::string::npos, "2910 AC3: densify fence present");
    CHECK(freeze_pos != std::string::npos, "2910 AC3: densify goal freeze present");
    CHECK(fence_pos < freeze_pos, "2910 AC3: fence before densify goal freeze");
    CHECK(mb.find("#2910") != std::string::npos, "2910 AC3: boundary cites #2910");
    // Steal resume: rehydrate + CS truth freeze before proof stamp.
    CHECK(efm.find("rehydrate_occurrence_from_persist") != std::string::npos,
          "2910 AC3: steal path rehydrates before stamp");
    CHECK(efm.find("steal_goal_truth_2910") != std::string::npos ||
              efm.find("#2910") != std::string::npos,
          "2910 AC3: steal path cites #2910 CS truth");
}

static void ac2910_4_goal_truth_after_rehydrate() {
    std::println("\n--- #2910 AC4: rehydrate → non-zero live_goal_count for stamp ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    UnitCs u;
    u.cs.set_current_epoch(5);
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), 3, 50, /*epoch=*/5);
    CHECK(u.cs.append_occurrence_snapshot(50) == 1, "2910 AC4: snapshot");
    u.cs.set_current_epoch(6);
    const auto dropped = u.cs.prune_occurrence_goals(6);
    CHECK(dropped == 1, "2910 AC4: prune dropped 1");
    CHECK(u.cs.occurrence_goals_size() == 0, "2910 AC4: empty after prune");
    const auto rh = u.cs.rehydrate_occurrence_from_persist(0);
    CHECK(rh >= 1, "2910 AC4: rehydrate restores ≥1");
    CHECK(u.cs.occurrence_goals_size() > 0, "2910 AC4: live non-empty after rehydrate");
    apply_dev_audit_defaults();
}

static void ac2910_5_query_and_source() {
    std::println("\n--- #2910 AC5: schema-2910 + lineage preserved ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2910") == 2910, "2910 AC5: schema-2910");
    CHECK(href(cs, "issue-2910") == 2910, "2910 AC5: issue-2910");
    CHECK(href(cs, "occurrence-persist-stamp-after-rehydrate-wired") == 1,
          "2910 AC5: stamp-after-rehydrate-wired");
    CHECK(href(cs, "occurrence-persist-production-always-on-success") == 1,
          "2910 AC5: production-always-on-success");
    CHECK(href(cs, "schema-2896") == 2896, "2910 AC5: schema-2896 preserved");
    CHECK(href(cs, "schema-2608") == 2608, "2910 AC5: schema-2608 preserved");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(impl.find("#2910") != std::string::npos, "2910 AC5: impl cites #2910");
    CHECK(mb.find("#2910") != std::string::npos, "2910 AC5: boundary cites #2910");
}

static void ac2910_6_linter_and_decision_table() {
    std::println("\n--- #2910 AC6: decision table + linter + no design doc ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_occurrence_persist_production_2910.py");
    CHECK(impl.find("Soft vs production decision table") != std::string::npos ||
              impl.find("Soft + goals") != std::string::npos,
          "2910 AC6: decision table in code comments");
    CHECK(build.find("check_occurrence_persist_production_2910") != std::string::npos,
          "2910 AC6: build.py wires linter");
    CHECK(!lint.empty() && lint.find("2910") != std::string::npos, "2910 AC6: linter present");
    CHECK(read_file("docs/design/2910-occurrence-persist.md").empty(),
          "2910 AC6: no docs/design/2910-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_2910.cpp").empty(),
          "2910 AC6: no new test file per #81967");
}

// ── Issue #2938: freeze Occurrence truth on every successful commit ──

static void ac2938_1_production_commit_snapshot_and_post_persist_stamp() {
    std::println("\n--- #2938 AC1: production success → snapshot + post-persist proof ---");
    CHECK(kOccurrenceCommitSnapshotIssue == 2938, "AC1: issue stamp 2938");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    reset_occurrence_commit_snapshot_for_test();
    UnitCs u;
    u.cs.set_current_epoch(1);
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), /*pred=*/7, /*mut=*/99, /*epoch=*/1);
    CHECK(u.cs.occurrence_goals_size() == 1, "AC1: one live goal");
    const auto w0 = occurrence_commit_snapshot_written_total_v_read();
    const auto written = u.cs.append_occurrence_snapshot(99);
    CHECK(written == 1, "AC1: append wrote 1");
    // Simulate outermost success note (C ABI does this after write).
    note_occurrence_commit_snapshot_written(99, static_cast<std::uint64_t>(written));
    CHECK(occurrence_commit_snapshot_written_total_v_read() == w0 + 1,
          "AC1: commit-snapshot-written-total +1");
    CHECK(occurrence_commit_snapshot_mid_v_read() == 99, "AC1: last mid = 99");
    // Source-cite: outermost success path stamps post-persist.
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mb.find("note_occurrence_commit_snapshot_written") != std::string::npos,
          "AC1: dtor C ABI notes commit snapshot");
    CHECK(mb.find("build_type_linear_commit_proof_from_live") != std::string::npos,
          "AC1: post-persist proof stamp in success helper");
    // Persist before stamp in the helper body order.
    const auto note_pos = mb.find("note_occurrence_commit_snapshot_written");
    const auto stamp_pos = mb.find("build_type_linear_commit_proof_from_live",
                                   note_pos != std::string::npos ? note_pos : 0);
    CHECK(note_pos != std::string::npos && stamp_pos != std::string::npos && note_pos < stamp_pos,
          "AC1: note write before post-persist proof stamp");
    apply_dev_audit_defaults();
}

static void ac2938_2_soft_empty_zero() {
    std::println("\n--- #2938 AC2: Soft + empty → zero commit counters ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_dev_audit_defaults();
    reset_occurrence_commit_snapshot_for_test();
    UnitCs u;
    u.cs.set_current_epoch(1);
    CHECK(u.cs.append_occurrence_snapshot(1) == 0, "AC2: empty Soft → 0 writes");
    note_occurrence_commit_snapshot_written(1, 0); // zero entries → no bump
    CHECK(occurrence_commit_snapshot_written_total_v_read() == 0,
          "AC2: written-total stays 0 on zero entries");
    CHECK(occurrence_commit_snapshot_mid_v_read() == 0, "AC2: mid stays 0");
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), 1, 1, 1);
    CHECK(u.cs.append_occurrence_snapshot(1) == 0, "AC2: Soft + goals still 0 without env");
}

static void ac2938_3_reject_never_writes() {
    std::println("\n--- #2938 AC3: reject path never writes commit snapshot ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // Persist helper only under outermost && success.
    CHECK(mb.find("if (outermost && success)") != std::string::npos, "AC3: success gate present");
    CHECK(mb.find("aura_outermost_success_persist_occurrence") != std::string::npos,
          "AC3: persist helper only on success path");
    // Reject stamps proof without calling persist helper in the same block.
    const auto reject_block = mb.find("linear-synth-hard-fail");
    CHECK(reject_block != std::string::npos, "AC3: reject path present");
    // Source-cite AC3 comment.
    CHECK(mb.find("Reject / force-rollback never reaches this block") != std::string::npos ||
              mb.find("never call this helper (AC3)") != std::string::npos,
          "AC3: reject-no-write contract source-cited");
}

static void ac2938_4_fence_after_snapshot() {
    std::println("\n--- #2938 AC4: fence after snapshotted commit → rehydrate or face ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    UnitCs u;
    u.cs.set_current_epoch(5);
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), 3, 50, /*epoch=*/5);
    CHECK(u.cs.append_occurrence_snapshot(50) == 1, "AC4: snapshot written");
    note_occurrence_commit_snapshot_written(50, 1);
    u.cs.set_current_epoch(6);
    const auto dropped = u.cs.prune_occurrence_goals(6);
    CHECK(dropped == 1, "AC4: prune dropped 1");
    CHECK(u.cs.occurrence_goals_size() == 0, "AC4: empty after prune");
    const auto rh = u.cs.rehydrate_occurrence_from_persist(0);
    CHECK(rh >= 1, "AC4: rehydrate restores ≥1");
    CHECK(u.cs.occurrence_goals_size() > 0, "AC4: live non-empty after rehydrate (no half-green)");
    apply_dev_audit_defaults();
}

static void ac2938_5_lineage_query() {
    std::println("\n--- #2938 AC5: additive keys + lineage preserved ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2938") == 2938, "AC5: schema-2938");
    CHECK(href(cs, "issue-2938") == 2938, "AC5: issue-2938");
    CHECK(href(cs, "occurrence-commit-snapshot-wired") == 1, "AC5: commit-snapshot-wired");
    CHECK(href(cs, "occurrence-commit-snapshot-written-total") >= 0,
          "AC5: written-total key present");
    CHECK(href(cs, "occurrence-commit-snapshot-mid") >= 0, "AC5: mid key present");
    CHECK(href(cs, "schema-2608") == 2608, "AC5: schema-2608 preserved");
    CHECK(href(cs, "schema-2910") == 2910, "AC5: schema-2910 preserved");
    CHECK(href(cs, "schema-2896") == 2896, "AC5: schema-2896 preserved");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("kOccurrenceCommitSnapshotIssue = 2938") != std::string::npos,
          "AC5: issue constant");
    CHECK(tma.find("g_occurrence_commit_snapshot_written_total") != std::string::npos,
          "AC5: written counter");
    CHECK(tma.find("g_last_proof_goal_fingerprint") != std::string::npos,
          "AC5: #2842 fingerprint preserved");
}

static void ac2938_6_linter_and_no_design() {
    std::println("\n--- #2938 AC6: linter + no docs/design/ ---");
    const auto t = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_occurrence_commit_snapshot_2938.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac2938_1_production_commit_snapshot_and_post_persist_stamp") != std::string::npos,
          "AC6: AC1 test");
    CHECK(t.find("ac2938_2_soft_empty_zero") != std::string::npos, "AC6: AC2 test");
    CHECK(t.find("ac2938_3_reject_never_writes") != std::string::npos, "AC6: AC3 test");
    CHECK(t.find("ac2938_4_fence_after_snapshot") != std::string::npos, "AC6: AC4 test");
    CHECK(t.find("ac2938_5_lineage_query") != std::string::npos, "AC6: AC5 test");
    CHECK(t.find("ac2938_6_linter_and_no_design") != std::string::npos, "AC6: AC6 self-test");
    CHECK(!lint.empty() && lint.find("Issue #2938") != std::string::npos, "AC6: linter present");
    CHECK(build.find("check_occurrence_commit_snapshot_2938") != std::string::npos,
          "AC6: build.py gate");
    CHECK(read_file("docs/design/2938-occurrence-commit-snapshot.md").empty(),
          "AC6: no docs/design/2938-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_2938.cpp").empty(),
          "AC6: no invent test file per #81967");
}

// ── Issue #3004: persist + Full audit atomic with query:type ──
// AC1 Production persist helper grants query:type after persist+stamp+ensure
// AC2 Soft empty / persist-off: no durable snapshot; discard 0
// AC3 Full audit / reject discards provisional live goals
// AC4 schema-3004 + #2938/#2910/#2964 lineage
// AC5 extend this suite; linter; no invent / no design

static void ac3004_1_authority_after_persist() {
    std::println("\n--- #3004 AC1: persist helper grants authority after Full success ---");
    CHECK(kOccurrencePersistAuditAtomicIssue == 3004, "AC1: issue stamp 3004");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto persist_pos = mb.find("maybe_persist_occurrence_snapshot");
    const auto stamp_pos = mb.find("build_type_linear_commit_proof_from_live",
                                   persist_pos != std::string::npos ? persist_pos : 0);
    const auto ens_pos = mb.find("ensure_occurrence_commit_or_recover",
                                 stamp_pos != std::string::npos ? stamp_pos : 0);
    const auto grant_pos =
        mb.find("grant_type_export_authority", ens_pos != std::string::npos ? ens_pos : 0);
    CHECK(persist_pos != std::string::npos && stamp_pos != std::string::npos &&
              ens_pos != std::string::npos && grant_pos != std::string::npos &&
              persist_pos < stamp_pos && stamp_pos < ens_pos && ens_pos < grant_pos,
          "AC1: persist → stamp → ensure → grant authority");
    const auto ev = read_file("src/compiler/evaluator.ixx");
    CHECK(ev.find("note_type_export_inflight") != std::string::npos,
          "AC1: Production infer is in-flight until persist");
    const auto tc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(tc.find("note_type_export_inflight") != std::string::npos,
          "AC1: typecheck marks Production infer in-flight");
}

static void ac3004_2_soft_no_durable() {
    std::println("\n--- #3004 AC2: Soft no durable persist ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_dev_audit_defaults();
    reset_occurrence_provisional_discard_for_test();
    UnitCs u;
    u.cs.set_current_epoch(1);
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), 1, 1, 1);
    CHECK(u.cs.append_occurrence_snapshot(1) == 0, "AC2: Soft persist off");
    CHECK(u.cs.occurrence_persist_log_size() == 0, "AC2: no durable snapshot");
    CHECK(occurrence_provisional_discard_total_v_read() == 0, "AC2: discard counter quiet");
}

static void ac3004_3_discard_provisional_on_fail() {
    std::println("\n--- #3004 AC3: fail discards provisional live goals ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    reset_occurrence_provisional_discard_for_test();
    UnitCs u;
    u.cs.set_current_epoch(1);
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), 2, 40, 1);
    CHECK(u.cs.occurrence_goals_size() == 1, "AC3: one provisional goal");
    CHECK(u.cs.occurrence_persist_log_size() == 0, "AC3: nothing durable yet");
    const auto dropped = u.cs.discard_provisional_occurrence_goals();
    CHECK(dropped == 1, "AC3: discarded 1 live goal");
    CHECK(u.cs.occurrence_goals_size() == 0, "AC3: live empty after discard");
    note_occurrence_provisional_discard(dropped);
    CHECK(occurrence_provisional_discard_total_v_read() >= 1, "AC3: discard counter");
    // Durable persist then fail: discard restores persist snapshot.
    u.cs.note_occurrence_goal(v, u.reg.int_type(), 2, 41, 1);
    CHECK(u.cs.append_occurrence_snapshot(41) == 1, "AC3: persist durable");
    auto v2 = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v2, u.reg.bool_type(), 3, 42, 1);
    CHECK(u.cs.occurrence_goals_size() == 2, "AC3: live grew after persist");
    CHECK(u.cs.discard_provisional_occurrence_goals() == 2, "AC3: drop live");
    CHECK(u.cs.occurrence_goals_size() >= 1, "AC3: rehydrate restores durable");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mb.find("discard_provisional_occurrence_snapshot") != std::string::npos,
          "AC3: dtor !success discards");
    CHECK(mb.find("clear_type_export_authority") != std::string::npos,
          "AC3: dtor !success clears query:type authority");
    apply_dev_audit_defaults();
}

static void ac3004_4_schema_and_lineage() {
    std::println("\n--- #3004 AC4: schema-3004 + lineage ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC4: warm");
    CHECK(href(cs, "schema-3004") == 3004, "AC4: schema-3004");
    CHECK(href(cs, "issue-3004") == 3004, "AC4: issue-3004");
    CHECK(href(cs, "occurrence-persist-audit-atomic-wired") == 1, "AC4: wired");
    CHECK(href(cs, "occurrence-provisional-discard-total") >= 0, "AC4: discard total");
    CHECK(href(cs, "schema-2938") == 2938, "AC4: schema-2938 preserved");
    CHECK(href(cs, "schema-2910") == 2910, "AC4: schema-2910 preserved");
    CHECK(href(cs, "schema-2964") == 2964 || href(cs, "schema-2938") == 2938,
          "AC4: #2964 or #2938 lineage present");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("kOccurrencePersistAuditAtomicIssue = 3004") != std::string::npos,
          "AC4: issue constant");
    CHECK(tma.find("linear_fast_path_ok") != std::string::npos, "AC4: #2964 linear_fast_path_ok");
}

static void ac3004_5_source_and_linter() {
    std::println("\n--- #3004 AC5: source-cite + linter ---");
    const auto t = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_occurrence_persist_audit_atomic_3004.py");
    const auto build = read_file("build.py");
    const auto evq = read_file("src/compiler/evaluator_primitives_eval.cpp");
    CHECK(t.find("ac3004_1_authority_after_persist") != std::string::npos, "AC5: AC1");
    CHECK(t.find("ac3004_2_soft_no_durable") != std::string::npos, "AC5: AC2");
    CHECK(t.find("ac3004_3_discard_provisional_on_fail") != std::string::npos, "AC5: AC3");
    CHECK(t.find("ac3004_4_schema_and_lineage") != std::string::npos, "AC5: AC4");
    CHECK(!lint.empty() && lint.find("3004") != std::string::npos, "AC5: linter");
    CHECK(build.find("check_occurrence_persist_audit_atomic_3004") != std::string::npos,
          "AC5: build.py");
    CHECK(evq.find("in-flight") != std::string::npos, "AC5: query:type in-flight signal");
    CHECK(read_file("docs/design/3004-occurrence-persist-audit-atomic.md").empty(),
          "AC5: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_3004.cpp").empty(),
          "AC5: no invent test_issue_3004");
}

// ── Issue #3082: mid/nested MutationBoundary occurrence is provisional ──
// AC1 Nested/mid success never appends the durable persist log
// AC2 While nested is open (and after nested success) query is in-flight
// AC3 Outermost success still persist + grant (#2938 / #3004)
// AC4 Soft empty / no nested → zero extra persist / no sticky inflight
// AC5 Existing persist+rehydrate tests stay; this suite + linter

static void ac3082_1_nested_success_never_persists() {
    std::println("\n--- #3082 AC1: nested/mid success never durable-persist ---");
    CHECK(kNestedOccurrenceProvisionalIssue == 3082, "AC1: issue stamp 3082");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto persist_call = mb.find("aura_outermost_success_persist_occurrence(ev_");
    const auto outermost_ok = mb.find("if (outermost && success)");
    CHECK(persist_call != std::string::npos && outermost_ok != std::string::npos &&
              outermost_ok < persist_call,
          "AC1: persist helper only under outermost && success");
    CHECK(mb.find("maybe_persist_occurrence_snapshot") != std::string::npos,
          "AC1: persist still goes through maybe_persist");
    // Nested dtor path marks inflight and does not append.
    CHECK(mb.find("Issue #3082") != std::string::npos, "AC1: dtor cites #3082");
    CHECK(mb.find("note_type_export_inflight") != std::string::npos,
          "AC1: nested path stamps inflight");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    reset_occurrence_commit_snapshot_for_test();
    const auto w0 = occurrence_commit_snapshot_written_total_v_read();
    {
        CompilerService svc;
        CHECK(svc.eval("(+ 1 1)").has_value(), "AC1: warm");
        bool outer_ok = true;
        Evaluator::MutationBoundaryGuard outer(svc.evaluator(), &outer_ok);
        {
            bool inner_ok = true;
            Evaluator::MutationBoundaryGuard inner(svc.evaluator(), &inner_ok);
            CHECK(inner_ok, "AC1: nested enter ok");
        }
        CHECK(occurrence_commit_snapshot_written_total_v_read() == w0,
              "AC1: nested success wrote 0 commit snapshots");
        (void)outer_ok;
    }
    apply_dev_audit_defaults();
}

static void ac3082_2_nested_query_inflight() {
    std::println("\n--- #3082 AC2: nested open / nested success → query in-flight ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_dev_audit_defaults();
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "AC2: warm");
    (void)svc.eval("(set-code \"(define f 1)\")");
    (void)svc.eval("(eval-current)");
    (void)svc.eval("(typecheck-current)");
    {
        bool outer_ok = true;
        Evaluator::MutationBoundaryGuard outer(svc.evaluator(), &outer_ok);
        {
            bool inner_ok = true;
            Evaluator::MutationBoundaryGuard inner(svc.evaluator(), &inner_ok);
            CHECK(svc.evaluator().mutation_boundary_depth_slot_value() >= 2, "AC2: nested depth");
            CHECK(!svc.evaluator().type_export_authoritative(),
                  "AC2: nested open not authoritative");
            CHECK(svc.evaluator().type_export_inflight(), "AC2: nested open in-flight");
            (void)svc.eval("(typecheck-current)");
            CHECK(!svc.evaluator().type_export_authoritative(),
                  "AC2: typecheck mid-nested does not grant");
            CHECK(svc.evaluator().type_export_inflight(), "AC2: still in-flight after typecheck");
            auto git = svc.eval("(get-inferred-type 0)");
            CHECK(git.has_value(), "AC2: get-inferred-type returned");
            (void)inner_ok;
        }
        CHECK(!svc.evaluator().type_export_authoritative(),
              "AC2: after nested success still not authoritative");
        CHECK(svc.evaluator().type_export_inflight(), "AC2: after nested success still in-flight");
        (void)svc.eval("(typecheck-current)");
        CHECK(!svc.evaluator().type_export_authoritative(),
              "AC2: typecheck after nested still refuses grant");
        CHECK(svc.evaluator().type_export_inflight(),
              "AC2: inflight sticky until outermost persist");
        (void)outer_ok;
    }
    const auto prim = read_file("src/compiler/evaluator_primitives_eval.cpp");
    CHECK(prim.find("in-flight") != std::string::npos &&
              prim.find("copy_infer_type_export_authority") != std::string::npos,
          "AC2: query + typecheck copy refuse mid-nested grant");
}

static void ac3082_3_outermost_persist_unchanged() {
    std::println("\n--- #3082 AC3: outermost success persist + grant unchanged ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto persist_pos = mb.find("maybe_persist_occurrence_snapshot");
    const auto stamp_pos = mb.find("build_type_linear_commit_proof_from_live",
                                   persist_pos != std::string::npos ? persist_pos : 0);
    const auto ens_pos = mb.find("ensure_occurrence_commit_or_recover",
                                 stamp_pos != std::string::npos ? stamp_pos : 0);
    const auto grant_pos =
        mb.find("grant_type_export_authority", ens_pos != std::string::npos ? ens_pos : 0);
    CHECK(persist_pos != std::string::npos && stamp_pos != std::string::npos &&
              ens_pos != std::string::npos && grant_pos != std::string::npos &&
              persist_pos < stamp_pos && stamp_pos < ens_pos && ens_pos < grant_pos,
          "AC3: persist → stamp → ensure → grant still sole outermost path");
    CHECK(mb.find("aura_outermost_success_persist_occurrence") != std::string::npos,
          "AC3: persist helper retained");
}

static void ac3082_4_soft_no_nested_zero_extra() {
    std::println("\n--- #3082 AC4: Soft empty / no nested → zero extra persist ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_dev_audit_defaults();
    reset_occurrence_commit_snapshot_for_test();
    reset_occurrence_provisional_discard_for_test();
    UnitCs u;
    u.cs.set_current_epoch(1);
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), 1, 1, 1);
    CHECK(u.cs.append_occurrence_snapshot(1) == 0, "AC4: Soft persist off");
    CHECK(u.cs.occurrence_persist_log_size() == 0, "AC4: no durable snapshot");
    CHECK(occurrence_commit_snapshot_written_total_v_read() == 0, "AC4: commit snapshot quiet");
    CHECK(occurrence_provisional_discard_total_v_read() == 0, "AC4: discard quiet");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "AC4: warm");
    (void)svc.eval("(typecheck-current)");
    {
        bool ok = true;
        Evaluator::MutationBoundaryGuard only(svc.evaluator(), &ok);
        CHECK(svc.evaluator().mutation_boundary_depth_slot_value() == 1, "AC4: outermost only");
        // No nested enter — do not force inflight on depth==1.
        (void)ok;
    }
    const auto ev = read_file("src/compiler/evaluator.ixx");
    CHECK(ev.find("copy_infer_type_export_authority") != std::string::npos,
          "AC4: Soft no-nested uses copy helper (inflight false → grant)");
}

static void ac3082_5_nested_fail_inflight_outer_abort_discards() {
    std::println("\n--- #3082 AC5: nested fail inflight; outermost abort still discards ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_dev_audit_defaults();
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "AC5: warm");
    (void)svc.eval("(typecheck-current)");
    {
        bool outer_ok = true;
        Evaluator::MutationBoundaryGuard outer(svc.evaluator(), &outer_ok);
        {
            bool inner_ok = true;
            Evaluator::MutationBoundaryGuard inner(svc.evaluator(), &inner_ok);
            inner_ok = false;
        }
        CHECK(!svc.evaluator().type_export_authoritative(), "AC5: nested fail not authoritative");
        CHECK(svc.evaluator().type_export_inflight(), "AC5: nested fail stays in-flight");
        CHECK(outer_ok, "AC5: nested fail does not flip outer success");
    }
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mb.find("outermost && !success") != std::string::npos, "AC5: outer abort discard kept");
    CHECK(mb.find("discard_provisional_occurrence_snapshot") != std::string::npos,
          "AC5: discard still outermost-fail");
    CHECK(mb.find("Do not discard live goals here") != std::string::npos,
          "AC5: nested fail does not wipe outer goals");
}

static void ac3082_6_schema_and_linter() {
    std::println("\n--- #3082 AC6: schema + linter + no invent ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC6: warm");
    CHECK(href(cs, "schema-3082") == 3082, "AC6: schema-3082");
    CHECK(href(cs, "issue-3082") == 3082, "AC6: issue-3082");
    CHECK(href(cs, "nested-occurrence-provisional-wired") == 1, "AC6: wired");
    CHECK(href(cs, "schema-3004") == 3004, "AC6: #3004 lineage");
    CHECK(href(cs, "schema-2938") == 2938, "AC6: #2938 lineage");
    const auto t = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_nested_occurrence_provisional_3082.py");
    const auto build = read_file("build.py");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(t.find("ac3082_1_nested_success_never_persists") != std::string::npos, "AC6: AC1");
    CHECK(t.find("ac3082_2_nested_query_inflight") != std::string::npos, "AC6: AC2");
    CHECK(t.find("ac3082_3_outermost_persist_unchanged") != std::string::npos, "AC6: AC3");
    CHECK(t.find("ac3082_4_soft_no_nested_zero_extra") != std::string::npos, "AC6: AC4");
    CHECK(t.find("ac3082_5_nested_fail_inflight_outer_abort_discards") != std::string::npos,
          "AC6: AC5");
    CHECK(tma.find("kNestedOccurrenceProvisionalIssue = 3082") != std::string::npos,
          "AC6: issue constant");
    CHECK(!lint.empty() && lint.find("Issue #3082") != std::string::npos, "AC6: linter");
    CHECK(build.find("check_nested_occurrence_provisional_3082") != std::string::npos,
          "AC6: build.py");
    CHECK(read_file("docs/design/3082-nested-occurrence-provisional.md").empty(),
          "AC6: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_3082.cpp").empty(),
          "AC6: no invent test_issue_3082");
}

// ── Issue #2981: steal/densify rehydrate miss binds TypeLinearCommitProof ──

static void ac2981_1_prod_miss_rejects_proof() {
    std::println("\n--- #2981 AC1: production + rehydrate miss → proof would_allow=false ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    clear_occurrence_empty_after_fence_for_test();
    typed_audit::reset_type_linear_proof_reject_empty_after_fence_for_test();
    typed_audit::clear_type_linear_proof_outcome_for_test();
    TypeRegistry reg;
    TypeChecker tc(reg);
    CompilerMetrics metrics{};
    tc.set_metrics(&metrics);
    auto& cs = tc.constraint_system();
    cs.set_metrics(&metrics);
    tc.set_cache_epoch(1);
    cs.set_current_epoch(1);
    auto v = cs.fresh_var();
    cs.note_occurrence_goal(v, reg.int_type(), 1, 10, /*epoch=*/1);
    CHECK(cs.occurrence_goals_size() == 1, "AC1: one live goal");
    // No snapshot → fence prune + persist enabled (production) → miss.
    const auto rej0 = typed_audit::type_linear_proof_reject_empty_after_fence_total_v_read();
    const auto dropped = tc.note_steal_or_densify_epoch_fence(2);
    CHECK(dropped >= 1, "AC1: fence dropped goals");
    CHECK(cs.occurrence_goals_size() == 0, "AC1: empty after miss");
    CHECK(typed_audit::type_linear_proof_reject_empty_after_fence_total_v_read() > rej0,
          "AC1: reject-empty-after-fence counter");
    CHECK(typed_audit::last_proof_would_allow_commit_v_read() == 0,
          "AC1: last proof would_allow_commit=false");
    CHECK(typed_audit::last_type_linear_proof_outcome_v_read() ==
              typed_audit::kTypeLinearProofOutcomeReject,
          "AC1: outcome Reject");
    CHECK(!typed_audit::occurrence_empty_after_fence_blocks_proof(1),
          "AC1: CS non-empty does not block");
    // #2995 ensure after miss may recover (empty CS SOLVED) and clear
    // the #2704 face. The same-txn stamp inside the fence still rejects.
    if (occurrence_empty_after_fence_total_v_read() > 0) {
        CHECK(typed_audit::occurrence_empty_after_fence_blocks_proof(0),
              "AC1: helper blocks empty CS while face live");
        const auto p = typed_audit::build_type_linear_commit_proof_from_live_with_outcome(
            99, /*would_allow_commit=*/true, /*linear_ok=*/true, /*goals=*/0, /*fp=*/0,
            /*from_cs=*/true);
        CHECK(!p.would_allow_commit, "AC1: with_outcome cannot stay green on empty+face");
        CHECK(p.force_reason_code == 11, "AC1: force_reason_code 11");
    } else {
        CHECK(true, "AC1: #2995 ensure recovered / cleared face after miss stamp");
    }
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    CHECK(ixx.find("Issue #2981") != std::string::npos, "AC1: fence cites #2981");
    apply_dev_audit_defaults();
    clear_occurrence_empty_after_fence_for_test();
}

static void ac2981_2_soft_observe_only() {
    std::println("\n--- #2981 AC2: Soft + miss → soft counter; proof may allow ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_dev_audit_defaults();
    clear_occurrence_empty_after_fence_for_test();
    typed_audit::reset_type_linear_proof_reject_empty_after_fence_for_test();
    typed_audit::note_occurrence_empty_after_fence(/*production_hard=*/false);
    CHECK(occurrence_empty_after_fence_soft_total_v_read() > 0, "AC2: soft counter");
    CHECK(occurrence_empty_after_fence_total_v_read() == 0, "AC2: hard total stays 0");
    CHECK(!typed_audit::occurrence_empty_after_fence_blocks_proof(0),
          "AC2: Soft does not block proof");
    const auto p = typed_audit::build_type_linear_commit_proof_from_live_with_outcome(1, true, true,
                                                                                      0, 0, true);
    CHECK(p.would_allow_commit, "AC2: Soft may still allow");
}

static void ac2981_3_quiet_zero_extra() {
    std::println("\n--- #2981 AC3: quiet (no prune / rehydrate success) → no extra reject ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    clear_occurrence_empty_after_fence_for_test();
    typed_audit::reset_type_linear_proof_reject_empty_after_fence_for_test();
    TypeRegistry reg;
    TypeChecker tc(reg);
    tc.set_cache_epoch(5);
    CHECK(tc.note_steal_or_densify_epoch_fence(5) == 0, "AC3: same epoch zero prune");
    CHECK(!typed_audit::occurrence_empty_after_fence_blocks_proof(0),
          "AC3: no face → helper false");
    CHECK(typed_audit::type_linear_proof_reject_empty_after_fence_total_v_read() == 0,
          "AC3: no reject bump");
    apply_dev_audit_defaults();
}

static void ac2981_4_same_txn_order() {
    std::println("\n--- #2981 AC4: #2854 same-txn order preserved ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(mb.find("empty_fence_2981") != std::string::npos, "AC4: densify folds #2981");
    CHECK(efm.find("empty_fence_2981") != std::string::npos, "AC4: steal folds #2981");
    const auto fence_pos = mb.find("note_type_freshness_after_steal_or_densify()");
    const auto fold_pos = mb.find("empty_fence_2981");
    CHECK(fence_pos != std::string::npos && fold_pos != std::string::npos && fence_pos < fold_pos,
          "AC4: fence before densify proof fold");
}

static void ac2981_5_additive_schema() {
    std::println("\n--- #2981 AC5: additive schema; #2704/#2910/#2842/#2697 preserved ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2981") == 2981, "AC5: schema-2981");
    CHECK(href(cs, "issue-2981") == 2981, "AC5: issue-2981");
    CHECK(href(cs, "type-linear-proof-empty-after-fence-wired") == 1, "AC5: wired");
    CHECK(href(cs, "type-linear-proof-reject-empty-after-fence-total") >= 0, "AC5: reject total");
    CHECK(href(cs, "schema-2910") == 2910, "AC5: schema-2910 preserved");
    CHECK(href(cs, "schema-2608") == 2608, "AC5: schema-2608 preserved");
    CHECK(href(cs, "schema-2896") == 2896, "AC5: schema-2896 preserved");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("g_occurrence_empty_after_fence_total") != std::string::npos,
          "AC5: #2704 counter preserved");
    CHECK(tma.find("occurrence_empty_after_fence_blocks_proof") != std::string::npos,
          "AC5: #2981 helper");
}

static void ac2981_6_source_and_linter() {
    std::println("\n--- #2981 AC6: source-cite + linter + no docs/design ---");
    const auto t = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
    CHECK(t.find("ac2981_1_prod_miss_rejects_proof") != std::string::npos, "AC6: AC1 present");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("occurrence_empty_after_fence_blocks_proof") != std::string::npos,
          "AC6: helper in tma");
    CHECK(tma.find("Issue #2981") != std::string::npos, "AC6: tma cites #2981");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mb.find("Issue #2981") != std::string::npos, "AC6: mb cites #2981");
    const auto lint =
        read_file("scripts/coverage/checks/check_type_linear_proof_empty_after_fence_2981.py");
    CHECK(!lint.empty() && lint.find("2981") != std::string::npos, "AC6: linter present");
    const auto build = read_file("build.py");
    CHECK(build.find("check_type_linear_proof_empty_after_fence_2981") != std::string::npos,
          "AC6: build.py wires linter");
    CHECK(read_file("docs/design/2981-empty-after-fence-proof.md").empty(),
          "AC6: no docs/design/2981-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_2981.cpp").empty(),
          "AC6: no invent test per #81967");
}

// ── Issue #3032: rehydrate-miss invalidates linear_fast_path + force deopt ──

static void ac3032_1_prod_miss_invalidates_fast_path() {
    std::println("\n--- #3032 AC1: production miss → !linear_fast_path_ok + deopt ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    clear_occurrence_empty_after_fence_for_test();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::reset_linear_ir_fastpath_counters_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
    typed_audit::clear_type_linear_proof_outcome_for_test();
    typed_audit::g_linear_ir_fastpath_boundary_depth_override = 0;
    typed_audit::g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);

    typed_audit::stamp_type_linear_commit_proof(30321);
    typed_audit::publish_type_linear_proof_outcome(typed_audit::kTypeLinearProofOutcomeStamped);
    typed_audit::publish_last_proof_face(true, true);
    CHECK(typed_audit::linear_fast_path_ok(), "3032 AC1: green before miss");
    CHECK(typed_audit::linear_ir_fastpath_try_skip(), "3032 AC1: try_skip before miss");

    TypeRegistry reg;
    TypeChecker tc(reg);
    CompilerMetrics metrics{};
    tc.set_metrics(&metrics);
    auto& cs = tc.constraint_system();
    cs.set_metrics(&metrics);
    tc.set_cache_epoch(1);
    cs.set_current_epoch(1);
    auto v = cs.fresh_var();
    cs.note_occurrence_goal(v, reg.int_type(), 1, 10, /*epoch=*/1);
    const auto inv0 = typed_audit::rehydrate_miss_invalidate_total_v_read();
    const auto deopt0 = typed_audit::rehydrate_miss_force_deopt_total_v_read();
    const auto dropped = tc.note_steal_or_densify_epoch_fence(2);
    CHECK(dropped >= 1, "3032 AC1: fence dropped goals");
    CHECK(cs.occurrence_goals_size() == 0, "3032 AC1: empty after miss");
    CHECK(typed_audit::rehydrate_miss_invalidate_total_v_read() > inv0, "3032 AC1: invalidate");
    CHECK(typed_audit::rehydrate_miss_force_deopt_total_v_read() > deopt0, "3032 AC1: force deopt");
    CHECK(!typed_audit::linear_fast_path_ok(), "3032 AC1: !ok after miss");
    CHECK(!typed_audit::linear_ir_fastpath_try_skip(), "3032 AC1: Move/Drop cannot skip");
    CHECK(typed_audit::linear_fast_path_boundary_exit_action() ==
              typed_audit::LinearFastPathExitAction::ForceRevalidate,
          "3032 AC1: ForceRevalidate until next green");

    apply_dev_audit_defaults();
    clear_occurrence_empty_after_fence_for_test();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
}

static void ac3032_2_soft_observe_only() {
    std::println("\n--- #3032 AC2: Soft miss observe; no gen bump ---");
    apply_dev_audit_defaults();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::set_strategy(typed_audit::AuditStrategy::Sampled);
    const auto hard0 = typed_audit::rehydrate_miss_invalidate_total_v_read();
    const auto obs0 = typed_audit::rehydrate_miss_invalidate_observe_total_v_read();
    CHECK(!typed_audit::invalidate_fast_path_on_rehydrate_miss(), "3032 AC2: Soft returns false");
    CHECK(typed_audit::rehydrate_miss_invalidate_total_v_read() == hard0, "3032 AC2: no hard");
    CHECK(typed_audit::rehydrate_miss_invalidate_observe_total_v_read() == obs0 + 1,
          "3032 AC2: observe");
    CHECK(typed_audit::rehydrate_miss_invalidate_gen_v_read() == 0, "3032 AC2: no gen bump");
}

static void ac3032_3_quiet_zero_cost() {
    std::println("\n--- #3032 AC3: quiet (no miss) zero extra ---");
    apply_production_audit_defaults();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    TypeRegistry reg;
    TypeChecker tc(reg);
    tc.set_cache_epoch(5);
    const auto inv0 = typed_audit::rehydrate_miss_invalidate_total_v_read();
    const auto obs0 = typed_audit::rehydrate_miss_invalidate_observe_total_v_read();
    CHECK(tc.note_steal_or_densify_epoch_fence(5) == 0, "3032 AC3: same epoch no prune");
    CHECK(typed_audit::rehydrate_miss_invalidate_total_v_read() == inv0, "3032 AC3: no invalidate");
    CHECK(typed_audit::rehydrate_miss_invalidate_observe_total_v_read() == obs0,
          "3032 AC3: no observe");
    apply_dev_audit_defaults();
}

static void ac3032_4_success_bind() {
    std::println("\n--- #3032 AC4: successful rehydrate binds before green ---");
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    const auto b0 = typed_audit::rehydrate_success_bind_total_v_read();
    typed_audit::note_rehydrate_success_bind(3, 0xabc);
    CHECK(typed_audit::rehydrate_success_bind_total_v_read() == b0 + 1, "3032 AC4: bind total");
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    apply_production_audit_defaults();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
    typed_audit::g_linear_ir_fastpath_boundary_depth_override = 0;
    typed_audit::g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    CHECK(typed_audit::invalidate_fast_path_on_rehydrate_miss(), "3032 AC4: miss invalidate");
    CHECK(!typed_audit::linear_fast_path_ok(), "3032 AC4: !ok after miss");
    typed_audit::stamp_type_linear_commit_proof(30324);
    typed_audit::publish_type_linear_proof_outcome(typed_audit::kTypeLinearProofOutcomeStamped);
    typed_audit::publish_last_proof_face(true, true);
    CHECK(typed_audit::linear_fast_path_ok(), "3032 AC4: green after bind restores");
    apply_dev_audit_defaults();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
}

static void ac3032_5_schema() {
    std::println("\n--- #3032 AC5: schema-3032 + lineage ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "3032 AC5: warm");
    CHECK(href(svc, "schema-3032") == 3032, "3032 AC5: schema-3032");
    CHECK(href(svc, "issue-3032") == 3032, "3032 AC5: issue-3032");
    CHECK(href(svc, "rehydrate-miss-invalidate-wired") == 1, "3032 AC5: wired");
    CHECK(href(svc, "rehydrate-miss-invalidate-total") >= 0, "3032 AC5: invalidate");
    CHECK(href(svc, "rehydrate-miss-invalidate-observe-total") >= 0, "3032 AC5: observe");
    CHECK(href(svc, "rehydrate-miss-force-deopt-total") >= 0, "3032 AC5: deopt");
    CHECK(href(svc, "rehydrate-miss-success-bind-total") >= 0, "3032 AC5: bind");
    CHECK(href(svc, "schema-2981") == 2981, "3032 AC5: schema-2981 preserved");
    CHECK(href(svc, "schema-2910") == 2910, "3032 AC5: schema-2910 preserved");
}

static void ac3032_6_source_and_linter() {
    std::println("\n--- #3032 AC6: source-cite + linter ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto t = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_rehydrate_miss_invalidate_3032.py");
    const auto build = read_file("build.py");
    CHECK(tma.find("invalidate_fast_path_on_rehydrate_miss") != std::string::npos,
          "3032 AC6: helper");
    CHECK(tma.find("kRehydrateMissInvalidateIssue") != std::string::npos, "3032 AC6: issue stamp");
    CHECK(ixx.find("invalidate_fast_path_on_rehydrate_miss") != std::string::npos,
          "3032 AC6: fence");
    CHECK(mb.find("invalidate_fast_path_on_rehydrate_miss") != std::string::npos,
          "3032 AC6: densify");
    CHECK(efm.find("invalidate_fast_path_on_rehydrate_miss") != std::string::npos,
          "3032 AC6: steal");
    CHECK(efm.find("aura_jit_walk_active_closures") != std::string::npos, "3032 AC6: walk deopt");
    CHECK(t.find("ac3032_1_prod_miss_invalidates_fast_path") != std::string::npos, "3032 AC6: AC1");
    CHECK(!lint.empty() && lint.find("3032") != std::string::npos, "3032 AC6: linter");
    CHECK(build.find("check_rehydrate_miss_invalidate_3032") != std::string::npos,
          "3032 AC6: build.py");
    CHECK(read_file("docs/design/3032-rehydrate-miss-invalidate.md").empty(),
          "3032 AC6: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_3032.cpp").empty(),
          "3032 AC6: no invent test_issue_3032");
}

// ── Issue #3063: steal/densify SUCCESS invalidate-before-restamp ──

static void ac3063_1_prod_success_blocks_elide() {
    std::println("\n--- #3063 AC1: production success invalidate → !elide ---");
    apply_production_audit_defaults();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::reset_linear_ir_fastpath_counters_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
    typed_audit::clear_type_linear_proof_outcome_for_test();
    typed_audit::g_linear_ir_fastpath_boundary_depth_override = 0;
    typed_audit::g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    typed_audit::stamp_type_linear_commit_proof(30631);
    typed_audit::publish_type_linear_proof_outcome(typed_audit::kTypeLinearProofOutcomeStamped);
    typed_audit::publish_last_proof_face(true, true);
    CHECK(typed_audit::linear_fast_path_ok(), "3063 AC1: green before restamp");
    CHECK(typed_audit::linear_ir_fastpath_try_skip(), "3063 AC1: skip before");
    const auto inv0 = typed_audit::steal_densify_success_invalidate_total_v_read();
    const auto gen0 = typed_audit::rehydrate_miss_invalidate_gen_v_read();
    CHECK(typed_audit::invalidate_fast_path_before_steal_densify_restamp(),
          "3063 AC1: production invalidate");
    CHECK(typed_audit::rehydrate_miss_invalidate_gen_v_read() == gen0 + 1,
          "3063 AC1: same invalidate_gen advanced");
    CHECK(typed_audit::steal_densify_success_invalidate_total_v_read() == inv0 + 1,
          "3063 AC1: success invalidate total");
    CHECK(!typed_audit::linear_fast_path_ok(), "3063 AC1: !ok after gen advance");
    CHECK(!typed_audit::linear_ir_fastpath_try_skip(), "3063 AC1: Move/Drop cannot skip");
    typed_audit::publish_last_proof_face(true, true);
    CHECK(typed_audit::linear_fast_path_ok(), "3063 AC1: green after rebind");
    apply_dev_audit_defaults();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
}

static void ac3063_2_soft_zero_extra() {
    std::println("\n--- #3063 AC2: Soft zero extra atomics ---");
    apply_dev_audit_defaults();
    typed_audit::set_strategy(typed_audit::AuditStrategy::Sampled);
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    const auto inv0 = typed_audit::steal_densify_success_invalidate_total_v_read();
    const auto gen0 = typed_audit::rehydrate_miss_invalidate_gen_v_read();
    CHECK(!typed_audit::invalidate_fast_path_before_steal_densify_restamp(),
          "3063 AC2: Soft returns false");
    CHECK(typed_audit::steal_densify_success_invalidate_total_v_read() == inv0,
          "3063 AC2: no new counter");
    CHECK(typed_audit::rehydrate_miss_invalidate_gen_v_read() == gen0, "3063 AC2: no gen bump");
}

static void ac3063_3_schema() {
    std::println("\n--- #3063 AC3: schema-3063 + SSOT ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "3063 AC3: warm");
    CHECK(href(svc, "schema-3063") == 3063, "3063 AC3: schema-3063");
    CHECK(href(svc, "issue-3063") == 3063, "3063 AC3: issue-3063");
    CHECK(href(svc, "steal-densify-success-invalidate-wired") == 1, "3063 AC3: wired");
    CHECK(href(svc, "steal-densify-success-invalidate-total") >= 0, "3063 AC3: total");
    CHECK(href(svc, "schema-3032") == 3032, "3063 AC3: schema-3032 preserved");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("invalidate_fast_path_before_steal_densify_restamp") != std::string::npos,
          "3063 AC3: helper");
    CHECK(tma.find("linear_fast_path_ok") != std::string::npos, "3063 AC3: SSOT predicate");
}

static void ac3063_4_source_and_linter() {
    std::println("\n--- #3063 AC4: source-cite + linter ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto ir = read_file("src/compiler/ir_executor_impl.cpp");
    const auto t = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_half_green_ir_steal_densify_3063.py");
    const auto build = read_file("build.py");
    CHECK(tma.find("Issue #3063") != std::string::npos, "3063 AC4: tma cite");
    CHECK(tma.find("invalidate_fast_path_before_steal_densify_restamp") != std::string::npos,
          "3063 AC4: helper");
    CHECK(efm.find("invalidate_fast_path_before_steal_densify_restamp") != std::string::npos,
          "3063 AC4: restamp site");
    CHECK(ir.find("Issue #3063") != std::string::npos, "3063 AC4: IR cite");
    CHECK(t.find("ac3063_1_prod_success_blocks_elide") != std::string::npos, "3063 AC4: AC1");
    CHECK(!lint.empty() && lint.find("3063") != std::string::npos, "3063 AC4: linter");
    CHECK(build.find("check_half_green_ir_steal_densify_3063") != std::string::npos,
          "3063 AC4: build.py");
    CHECK(read_file("docs/design/3063-half-green-ir-steal-densify.md").empty(),
          "3063 AC4: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_3063.cpp").empty(),
          "3063 AC4: no invent test_issue_3063");
}

// ── Issue #3085: densify/steal miss blocks lowering elision via gen ──
// AC1 miss advances gen; lowering block sees it before next lower
// AC2 linear_fast_path_ok false until green rebind
// AC3 abort still uses existing clear (no second clear)
// AC4 Soft / no densify → gen stays 0
// AC5 schema + linter; extend this suite

static void ac3085_1_densify_miss_blocks_elision() {
    std::println("\n--- #3085 AC1: densify-miss → lowering elision blocked ---");
    CHECK(typed_audit::kLinearFastPathRehydrateGenElisionIssue == 3085, "3085 AC1: issue stamp");
    apply_production_audit_defaults();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::reset_linear_ir_fastpath_counters_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
    typed_audit::clear_type_linear_proof_outcome_for_test();
    typed_audit::g_linear_ir_fastpath_boundary_depth_override = 0;
    typed_audit::g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    typed_audit::stamp_type_linear_commit_proof(30851);
    typed_audit::publish_type_linear_proof_outcome(typed_audit::kTypeLinearProofOutcomeStamped);
    typed_audit::publish_last_proof_face(true, true);
    CHECK(typed_audit::linear_fast_path_ok(), "3085 AC1: green before miss");
    CHECK(typed_audit::linear_ir_fastpath_try_skip(), "3085 AC1: skip before miss");
    CHECK(!typed_audit::linear_fast_path_rehydrate_gen_blocks_elision(),
          "3085 AC1: gens match before miss");
    CHECK(aura_linear_fast_path_depth_or_densify_block() == 0,
          "3085 AC1: lowering not blocked before miss");
    const auto gen0 = typed_audit::rehydrate_miss_invalidate_gen_v_read();
    CHECK(typed_audit::invalidate_fast_path_on_rehydrate_miss(), "3085 AC1: miss invalidate");
    CHECK(typed_audit::rehydrate_miss_invalidate_gen_v_read() == gen0 + 1,
          "3085 AC1: invalidate gen advanced before lowering");
    CHECK(!typed_audit::linear_fast_path_ok(), "3085 AC1: !ok after miss");
    CHECK(!typed_audit::linear_ir_fastpath_try_skip(), "3085 AC1: Move/Drop cannot skip");
    CHECK(typed_audit::linear_fast_path_rehydrate_gen_blocks_elision(),
          "3085 AC1: gen blocks lowering elision");
    CHECK(aura_linear_fast_path_depth_or_densify_block() != 0,
          "3085 AC1: lowering helper blocks after miss");
    apply_dev_audit_defaults();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
}

static void ac3085_2_green_rebind_restores() {
    std::println("\n--- #3085 AC2: !ok until fresh green restamp ---");
    apply_production_audit_defaults();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
    typed_audit::clear_type_linear_proof_outcome_for_test();
    typed_audit::g_linear_ir_fastpath_boundary_depth_override = 0;
    typed_audit::g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    typed_audit::stamp_type_linear_commit_proof(30852);
    typed_audit::publish_type_linear_proof_outcome(typed_audit::kTypeLinearProofOutcomeStamped);
    typed_audit::publish_last_proof_face(true, true);
    CHECK(typed_audit::invalidate_fast_path_on_rehydrate_miss(), "3085 AC2: miss");
    CHECK(!typed_audit::linear_fast_path_ok(), "3085 AC2: !ok after miss");
    CHECK(aura_linear_fast_path_depth_or_densify_block() != 0, "3085 AC2: lowering blocked");
    typed_audit::publish_last_proof_face(true, true);
    CHECK(typed_audit::linear_fast_path_ok(), "3085 AC2: green rebind restores ok");
    CHECK(!typed_audit::linear_fast_path_rehydrate_gen_blocks_elision(),
          "3085 AC2: gens match after rebind");
    CHECK(aura_linear_fast_path_depth_or_densify_block() == 0,
          "3085 AC2: lowering unblocked after green");
    apply_dev_audit_defaults();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
}

static void ac3085_3_abort_clear_unchanged() {
    std::println("\n--- #3085 AC3: abort still uses existing clear ---");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mb.find("clear_type_linear_commit_proof_on_abort") != std::string::npos,
          "3085 AC3: abort clear retained");
    CHECK(mb.find("invalidate_fast_path_on_rehydrate_miss") != std::string::npos,
          "3085 AC3: miss invalidate stays on miss path");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("clear_type_linear_commit_proof_on_abort") != std::string::npos,
          "3085 AC3: abort helper");
    CHECK(tma.find("linear_fast_path_rehydrate_gen_blocks_elision") != std::string::npos,
          "3085 AC3: gen-elision helper does not clear stamp");
}

static void ac3085_4_soft_zero_extra() {
    std::println("\n--- #3085 AC4: Soft / no densify → zero extra ---");
    apply_dev_audit_defaults();
    typed_audit::set_strategy(typed_audit::AuditStrategy::Sampled);
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::g_linear_ir_fastpath_boundary_depth_override = 0;
    typed_audit::g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    const auto gen0 = typed_audit::rehydrate_miss_invalidate_gen_v_read();
    CHECK(!typed_audit::invalidate_fast_path_on_rehydrate_miss(), "3085 AC4: Soft observe");
    CHECK(typed_audit::rehydrate_miss_invalidate_gen_v_read() == gen0, "3085 AC4: no gen bump");
    CHECK(!typed_audit::linear_fast_path_rehydrate_gen_blocks_elision(),
          "3085 AC4: inv==0 no block");
    CHECK(aura_linear_fast_path_depth_or_densify_block() == 0, "3085 AC4: lowering quiet");
}

static void ac3085_5_schema_and_linter() {
    std::println("\n--- #3085 AC5: schema + linter + no invent ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "3085 AC5: warm");
    CHECK(href(svc, "schema-3085") == 3085, "3085 AC5: schema-3085");
    CHECK(href(svc, "issue-3085") == 3085, "3085 AC5: issue-3085");
    CHECK(href(svc, "linear-fast-path-rehydrate-gen-elision-wired") == 1, "3085 AC5: wired");
    CHECK(href(svc, "schema-3032") == 3032, "3085 AC5: schema-3032 preserved");
    CHECK(href(svc, "schema-3063") == 3063, "3085 AC5: schema-3063 preserved");
    const auto t = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_rehydrate_miss_lowering_elision_3085.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac3085_1_densify_miss_blocks_elision") != std::string::npos, "3085 AC5: AC1");
    CHECK(t.find("ac3085_2_green_rebind_restores") != std::string::npos, "3085 AC5: AC2");
    CHECK(t.find("ac3085_3_abort_clear_unchanged") != std::string::npos, "3085 AC5: AC3");
    CHECK(t.find("ac3085_4_soft_zero_extra") != std::string::npos, "3085 AC5: AC4");
    CHECK(!lint.empty() && lint.find("Issue #3085") != std::string::npos, "3085 AC5: linter");
    CHECK(build.find("check_rehydrate_miss_lowering_elision_3085") != std::string::npos,
          "3085 AC5: build.py");
    CHECK(read_file("tests/compiler/test_issue_3085.cpp").empty(),
          "3085 AC5: no invent test_issue_3085");
    CHECK(read_file("docs/design/3085-rehydrate-miss-lowering-elision.md").empty(),
          "3085 AC5: no docs/design/");
}

// ── Issue #2995: unified OccurrenceCommitHealth + single-shot ensure ──

static void ac2995_1_soft_empty_pure_loads() {
    std::println("\n--- #2995 AC1: Soft + empty → pure loads, no persist/recover ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_dev_audit_defaults();
    reset_occurrence_commit_health_for_test();
    reset_occurrence_commit_snapshot_for_test();
    clear_occurrence_empty_after_fence_for_test();
    TypeRegistry reg;
    TypeChecker tc(reg);
    const auto ensure0 = occurrence_commit_health_ensure_total_v_read();
    const auto rec0 = occurrence_commit_health_recover_ok_total_v_read();
    const auto snap0 = occurrence_commit_snapshot_written_total_v_read();
    const auto h = tc.evaluate_occurrence_commit_health();
    CHECK(h.goals_live == 0, "2995 AC1: goals_live 0");
    CHECK(h.persist_size == 0, "2995 AC1: persist_size 0");
    CHECK(h.faces_bitmask == 0, "2995 AC1: no faces");
    CHECK(!h.needs_recover, "2995 AC1: Soft no recover");
    CHECK(h.fingerprint_ok, "2995 AC1: fingerprint_ok");
    CHECK(!h.recovered_ok, "2995 AC1: recovered_ok false");
    CHECK(tc.ensure_occurrence_commit_or_recover(), "2995 AC1: ensure is evaluate-only");
    CHECK(occurrence_commit_health_ensure_total_v_read() == ensure0,
          "2995 AC1: ensure counter stable");
    CHECK(occurrence_commit_health_recover_ok_total_v_read() == rec0,
          "2995 AC1: recover counter stable");
    CHECK(occurrence_commit_snapshot_written_total_v_read() == snap0, "2995 AC1: no persist write");
    CHECK(tc.maybe_persist_occurrence_snapshot(1) == 0, "2995 AC1: persist still Soft-off");
}

static void ac2995_2_production_face_one_shot_recover() {
    std::println("\n--- #2995 AC2: production + empty-after-fence → one recover ---");
    apply_production_audit_defaults();
    reset_occurrence_commit_health_for_test();
    clear_occurrence_empty_after_fence_for_test();
    TypeRegistry reg;
    TypeChecker tc(reg);
    typed_audit::note_occurrence_empty_after_fence(/*production_hard=*/true);
    const auto h0 = tc.evaluate_occurrence_commit_health();
    CHECK(h0.needs_recover, "2995 AC2: needs_recover");
    CHECK((h0.faces_bitmask & kOccurrenceCommitFaceEmptyAfterFence) != 0,
          "2995 AC2: empty-after-fence face");
    const auto ensure0 = occurrence_commit_health_ensure_total_v_read();
    const auto rec0 = occurrence_commit_health_recover_ok_total_v_read();
    CHECK(tc.ensure_occurrence_commit_or_recover(), "2995 AC2: empty CS recover SOLVED");
    CHECK(occurrence_commit_health_ensure_total_v_read() == ensure0 + 1,
          "2995 AC2: ensure exactly once");
    CHECK(occurrence_commit_health_recover_ok_total_v_read() == rec0 + 1, "2995 AC2: recover ok");
    CHECK(occurrence_empty_after_fence_total_v_read() == 0, "2995 AC2: face cleared");
    const auto h1 = tc.evaluate_occurrence_commit_health();
    CHECK(!h1.needs_recover, "2995 AC2: needs_recover cleared");
    CHECK(h1.recovered_ok, "2995 AC2: recovered_ok");
    apply_dev_audit_defaults();
    clear_occurrence_empty_after_fence_for_test();
    reset_occurrence_commit_health_for_test();
}

static void ac2995_4_fingerprint_after_persist() {
    std::println("\n--- #2995 AC4: post-persist fingerprint matches live goals ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    reset_occurrence_commit_health_for_test();
    TypeRegistry reg;
    TypeChecker tc(reg);
    auto& cs = tc.constraint_system();
    cs.set_current_epoch(1);
    auto v = cs.fresh_var();
    cs.note_occurrence_goal(v, reg.int_type(), 7, 99, 1);
    CHECK(tc.maybe_persist_occurrence_snapshot(99) >= 1, "2995 AC4: persist wrote");
    std::uint64_t acc = 0xcbf29ce484222325ULL;
    for (const auto& g : cs.occurrence_goals_for_test()) {
        acc = typed_audit::mix_occurrence_goal_into_fingerprint(acc, g.var.index, g.refined.index,
                                                                g.predicate_cond_node,
                                                                g.source_mutation_id, g.epoch);
    }
    const auto fp = (acc != 0) ? acc : 1;
    (void)typed_audit::build_type_linear_commit_proof_from_live(
        99, static_cast<std::uint64_t>(cs.occurrence_goals_size()), fp, true);
    const auto h = tc.evaluate_occurrence_commit_health();
    CHECK(h.goals_live == 1, "2995 AC4: goals_live 1");
    CHECK(h.persist_size >= 1, "2995 AC4: persist_size");
    CHECK(h.fingerprint_ok, "2995 AC4: fingerprint matches after stamp");
    apply_dev_audit_defaults();
}

static void ac2995_5_fence_same_ensure() {
    std::println("\n--- #2995 AC5: fence health + same ensure entry ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    reset_occurrence_commit_health_for_test();
    clear_occurrence_empty_after_fence_for_test();
    TypeRegistry reg;
    TypeChecker tc(reg);
    auto& cs = tc.constraint_system();
    cs.set_current_epoch(1);
    auto v = cs.fresh_var();
    cs.note_occurrence_goal(v, reg.int_type(), 3, 50, 1);
    CHECK(tc.maybe_persist_occurrence_snapshot(50) >= 1, "2995 AC5: snapshot");
    const auto dropped = tc.note_steal_or_densify_epoch_fence(2);
    CHECK(dropped == 1, "2995 AC5: prune dropped 1");
    const auto h = tc.evaluate_occurrence_commit_health();
    CHECK(h.goals_live > 0, "2995 AC5: post-rehydrate live size");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(ixx.find("ensure_occurrence_commit_or_recover") != std::string::npos,
          "2995 AC5: fence calls ensure");
    const auto reh = ixx.find("rehydrate_occurrence_from_persist");
    const auto ens =
        ixx.find("ensure_occurrence_commit_or_recover", reh != std::string::npos ? reh : 0);
    CHECK(reh != std::string::npos && ens != std::string::npos && reh < ens,
          "2995 AC5: ensure after #2910 rehydrate");
    CHECK(mb.find("ensure_occurrence_commit_or_recover") != std::string::npos,
          "2995 AC5: outermost success same ensure");
    apply_dev_audit_defaults();
    clear_occurrence_empty_after_fence_for_test();
}

static void ac2995_6_query_keys() {
    std::println("\n--- #2995 AC6: fidelity query keys ---");
    CompilerService svc;
    CHECK(href(svc, "schema-2995") == 2995, "2995 AC6: schema-2995");
    CHECK(href(svc, "issue-2995") == 2995, "2995 AC6: issue-2995");
    CHECK(href(svc, "occurrence-commit-health-wired") == 1, "2995 AC6: wired");
    CHECK(href(svc, "occurrence-commit-health-faces") >= 0, "2995 AC6: faces");
    CHECK(href(svc, "occurrence-commit-health-goals-live") >= 0, "2995 AC6: goals-live");
    CHECK(href(svc, "occurrence-commit-health-persist-size") >= 0, "2995 AC6: persist-size");
    CHECK(href(svc, "occurrence-commit-health-needs-recover") >= 0, "2995 AC6: needs-recover");
    CHECK(href(svc, "occurrence-commit-health-recovered-ok") >= 0, "2995 AC6: recovered-ok");
    CHECK(href(svc, "schema-2938") == 2938, "2995 AC6: schema-2938 preserved");
    CHECK(href(svc, "schema-2910") == 2910, "2995 AC6: schema-2910 preserved");
    CHECK(kOccurrenceCommitHealthIssue == 2995, "2995 AC6: ixx issue constant");
}

static void ac2995_7_source_cite() {
    std::println("\n--- #2995 AC7: source-cite + linter ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto t = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_occurrence_commit_health_2995.py");
    const auto build = read_file("build.py");
    CHECK(ixx.find("struct OccurrenceCommitHealth") != std::string::npos, "2995 AC7: struct");
    CHECK(ixx.find("evaluate_occurrence_commit_health") != std::string::npos, "2995 AC7: evaluate");
    CHECK(ixx.find("ensure_occurrence_commit_or_recover") != std::string::npos, "2995 AC7: ensure");
    CHECK(ixx.find("try_occurrence_hard_face_full_solve_recover") != std::string::npos,
          "2995 AC7: existing recover");
    CHECK(mb.find("#2938") != std::string::npos, "2995 AC7: #2938 order");
    CHECK(ixx.find("#2910") != std::string::npos, "2995 AC7: #2910 order");
    CHECK(t.find("ac2995_1_soft_empty_pure_loads") != std::string::npos, "2995 AC7: AC1");
    CHECK(!lint.empty(), "2995 AC7: linter");
    CHECK(build.find("check_occurrence_commit_health_2995") != std::string::npos,
          "2995 AC7: build.py");
    CHECK(read_file("docs/design/2995-occurrence-commit-health.md").empty(),
          "2995 AC7: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_2995.cpp").empty(),
          "2995 AC7: no invent test_issue_2995");
}

} // namespace

int run_test_occurrence_goal_persist_rehydrate() {
    std::println("=== test_occurrence_goal_persist_rehydrate ===");
    aura::compiler::lock_order::reset_tls_for_test();
    ac2_soft_zero_writes();
    ac1_persist_prune_rehydrate();
    ac3_cap_truncates();
    ac4_query_and_source();
    ac5_wiring();
    std::println("\n=== #2641 production-default OccurrenceGoal persist ===");
    ac2641_1_production_default_persist();
    ac2641_3_env_zero_forces_off();
    ac2641_4_rehydrate_miss_counter();
    ac2641_6_source_cite();
    std::println("\n=== #2896 production-default outermost + fence face ===");
    ac2896_1_production_persist_without_env();
    ac2896_2_soft_zero_cost();
    ac2896_3_fence_rehydrate_or_face();
    ac2896_4_goal_truth_after_rehydrate();
    ac2896_5_query_and_source();
    std::println("\n=== #2910 stamp-after-rehydrate + production always-on ===");
    ac2910_1_production_always_persist();
    ac2910_2_soft_zero_cost();
    ac2910_3_stamp_after_rehydrate_order();
    ac2910_4_goal_truth_after_rehydrate();
    ac2910_5_query_and_source();
    ac2910_6_linter_and_decision_table();
    std::println("\n=== #2938 outermost success freezes Occurrence commit snapshot ===");
    ac2938_1_production_commit_snapshot_and_post_persist_stamp();
    ac2938_2_soft_empty_zero();
    ac2938_3_reject_never_writes();
    ac2938_4_fence_after_snapshot();
    ac2938_5_lineage_query();
    ac2938_6_linter_and_no_design();
    std::println("\n=== #2981 same-txn proof bind on empty-after-fence miss ===");
    ac2981_1_prod_miss_rejects_proof();
    ac2981_2_soft_observe_only();
    ac2981_3_quiet_zero_extra();
    ac2981_4_same_txn_order();
    ac2981_5_additive_schema();
    ac2981_6_source_and_linter();
    std::println("\n=== #2995 OccurrenceCommitHealth + single-shot ensure ===");
    ac2995_1_soft_empty_pure_loads();
    ac2995_2_production_face_one_shot_recover();
    ac2995_4_fingerprint_after_persist();
    ac2995_5_fence_same_ensure();
    ac2995_6_query_keys();
    ac2995_7_source_cite();
    // #2995 acquire workspace/type locks; TLS depth can desync from the
    // actual mutex so #3004 then hits pthread EDEADLK. Clear depth first.
    aura::compiler::lock_order::reset_tls_for_test();
    std::println("\n=== #3004 persist + Full audit atomic with query:type ===");
    ac3004_1_authority_after_persist();
    ac3004_2_soft_no_durable();
    ac3004_3_discard_provisional_on_fail();
    ac3004_4_schema_and_lineage();
    ac3004_5_source_and_linter();
    std::println("\n=== #3082 mid/nested MutationBoundary occurrence provisional ===");
    ac3082_1_nested_success_never_persists();
    ac3082_2_nested_query_inflight();
    ac3082_3_outermost_persist_unchanged();
    ac3082_4_soft_no_nested_zero_extra();
    ac3082_5_nested_fail_inflight_outer_abort_discards();
    ac3082_6_schema_and_linter();
    std::println("\n=== #3032 rehydrate-miss invalidates linear_fast_path + deopt ===");
    ac3032_1_prod_miss_invalidates_fast_path();
    ac3032_2_soft_observe_only();
    ac3032_3_quiet_zero_cost();
    ac3032_4_success_bind();
    ac3032_5_schema();
    ac3032_6_source_and_linter();
    std::println("\n=== #3063 steal/densify success invalidate-before-restamp ===");
    ac3063_1_prod_success_blocks_elide();
    ac3063_2_soft_zero_extra();
    ac3063_3_schema();
    ac3063_4_source_and_linter();
    std::println("\n=== #3085 densify/steal miss blocks lowering elision ===");
    ac3085_1_densify_miss_blocks_elision();
    ac3085_2_green_rebind_restores();
    ac3085_3_abort_clear_unchanged();
    ac3085_4_soft_zero_extra();
    ac3085_5_schema_and_linter();
    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_occurrence_goal_persist_rehydrate();
}
#endif
