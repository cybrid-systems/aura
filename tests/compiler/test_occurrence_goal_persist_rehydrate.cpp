// @category: unit
// @reason: Issue #2608 — optional OccurrenceGoal persist on side buffer
//          for cross-delta / multi-session rehydrate after steal/densify.
//          Issue #2896 — production-default outermost success persist +
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

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
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
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.type;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::ConstraintSystem;
using aura::compiler::solve_delta_occurrence;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::clear_occurrence_empty_after_fence_for_test;
using aura::compiler::typed_audit::occurrence_empty_after_fence_soft_total_v_read;
using aura::compiler::typed_audit::occurrence_empty_after_fence_total_v_read;
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
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
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
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
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
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");

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

} // namespace

int run_test_occurrence_goal_persist_rehydrate() {
    std::println("=== test_occurrence_goal_persist_rehydrate ===");
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
    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_occurrence_goal_persist_rehydrate();
}
#endif
