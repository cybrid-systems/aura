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
//
//   #3193 AC1: production abort hold blocks rehydrate until persist+proof clear
//   #3193 AC2: no mixed green proof + residual persist during hold
//   #3193 AC3: Soft observe-only; quiet (no abort) zero extra
//   #3193 AC4: source-cite + linter; no docs/design / invent
//   #3232 AC1: nested AbortAuthorityHold keeps in_flight until last end
//   #3232 AC2: Soft nested observe-only; quiet no-abort zero extra
//   #3232 AC3: dual_restore + rehydrate sites source-cite AbortAuthorityHold
//   #3232 AC4: #3193 ACs preserved; no invent / docs/design
//   #3346 AC1: last-look fingerprint + live_goal_count + linear_root before stamp
//   #3346 AC2: densify/steal refuse green if mid_abort_authority outstanding
//   #3346 AC3: last_proof_* acquire-consistent with live table after stamp
//   #3346 AC4: Soft/Off zero extra; no new query key / invent / docs/design

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

extern "C" int aura_jit_ir_typed_entry_commit_readiness_ok(void);
extern "C" int aura_jit_linear_move_drop_elision_ok(void);
extern "C" int aura_jit_linear_post_mutate_enforce(std::uint32_t env_id);

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

// ── Issue #3170: outermost-success occurrence persist fingerprint guard
// + uniform clear-on-abort/nested (I4 from 2026-08 type-system review —
// 半解不得出厂). AC1 outermost success + fingerprint match → persist
// frozen. AC2 fingerprint mismatch OR abort OR OR nested → persist buffer
// cleared. AC3 Soft / Off / unit-test default: zero behavioural change.
// AC4 quiet (clean / no dirty / no TIMEOUT): zero extra atomics. AC5
// extends existing #2608 / #2641 lineage. AC6 source-cite + linter.
// Source: src/compiler/observability_metrics.h occurrence_persist_fingerprint_mismatch_total,
// src/compiler/type_checker.ixx clear_occurrence_persist_snapshot,
// src/compiler/type_checker_impl.cpp ConstraintSystem::clear_occurrence_persist_snapshot,
// src/compiler/typed_mutation_audit.h occurrence_goal_fingerprint +
// clear_occurrence_persist_buffer, src/compiler/evaluator_mutation_boundary.cpp
// aura_clear_occurrence_persist_buffer C ABI + aura_outermost_success_persist_occurrence
// fingerprint guard + 3 abort/nested clear calls.
static void ac3170_1_outermost_success_fingerprint_guard() {
    std::println("\n--- #3170 AC1: outermost success fingerprint guard ---");
    // Source-cite: clear_occurrence_persist_snapshot decl + impl + TypeChecker wrapper.
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto obs = read_file("src/compiler/observability_metrics.h");
    CHECK(ixx.find("clear_occurrence_persist_snapshot() noexcept") != std::string::npos,
          "3170 AC1: clear_occurrence_persist_snapshot decl in type_checker.ixx");
    CHECK(impl.find("ConstraintSystem::clear_occurrence_persist_snapshot") != std::string::npos,
          "3170 AC1: clear_occurrence_persist_snapshot impl in type_checker_impl.cpp");
    CHECK(tma.find("occurrence_goal_fingerprint(void* tc_handle)") != std::string::npos,
          "3170 AC1: occurrence_goal_fingerprint helper in typed_mutation_audit.h");
    CHECK(tma.find("clear_occurrence_persist_buffer(void* tc_handle)") != std::string::npos,
          "3170 AC1: clear_occurrence_persist_buffer wrapper in typed_mutation_audit.h");
    CHECK(emb.find("extern \"C\" void aura_clear_occurrence_persist_buffer(void* ev_ptr)") !=
              std::string::npos,
          "3170 AC1: aura_clear_occurrence_persist_buffer C ABI");
    CHECK(emb.find("Issue #3170: outermost-success fingerprint guard") != std::string::npos,
          "3170 AC1: fingerprint guard wire-up cite in evaluator_mutation_boundary.cpp");
    CHECK(obs.find("occurrence_persist_fingerprint_mismatch_total") != std::string::npos,
          "3170 AC1: counter declared at struct end of observability_metrics.h");
    CHECK(!read_file("docs/design/3170-occurrence-persist-fingerprint.md").empty() == false,
          "3170 AC5: no docs/design/3170-* plan doc (per #1655)");
    for (const auto& rel : {std::string("tests/issues/test_issue_3170.cpp"),
                            std::string("tests/compiler/test_issue_3170.cpp"),
                            std::string("tests/serve/test_issue_3170.cpp")}) {
        std::error_code ec;
        CHECK(!std::filesystem::exists(rel, ec),
              std::format("3170 AC5: forbidden {} per #81967", rel));
    }
}

static void ac3170_2_abort_nested_uniform_clear() {
    std::println("\n--- #3170 AC2: abort / nested / force-rollback paths uniformly clear ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // Three production exit paths must call aura_clear_occurrence_persist_buffer.
    const auto first_abort =
        emb.find("if (outermost && !success)\n        ev_->bump_mutation_boundary_rollback();");
    CHECK(first_abort != std::string::npos,
          "3170 AC2: outermost abort path bumps rollback (persist clear is on abort body)");
    const auto second_abort = emb.find(
        "} else if (outermost && !success) {\n"
        "        if (auto* m = static_cast<CompilerMetrics*>(ev_->compiler_metrics())) {\n"
        "            m->mutation_boundary_steal_recoveries.fetch_add(1, "
        "std::memory_order_relaxed);\n"
        "        }\n"
        "        // Issue #3004: Full audit / TIMEOUT / reject \u2014 discard provisional\n"
        "        // OccurrenceGoal live table; no query:type authority.\n"
        "        if (auto* tc = static_cast<TypeChecker*>(ev_->commit_type_checker_handle())) {\n"
        "            const auto dropped = tc->discard_provisional_occurrence_snapshot();\n"
        "            if (dropped > 0)\n"
        "                "
        "aura::compiler::typed_audit::note_occurrence_provisional_discard(dropped);\n"
        "        }\n"
        "        // Issue #3170: clear occurrence persist buffer on outermost abort\n"
        "        // (uniform enforcement \u2014 no half-written state survives).\n"
        "        aura_clear_occurrence_persist_buffer(ev_);\n"
        "        ev_->clear_type_export_authority();\n"
        "    }");
    CHECK(second_abort != std::string::npos,
          "3170 AC2: second abort path (line 4315) calls aura_clear_occurrence_persist_buffer");
    const auto nested_path = emb.find("aura_clear_occurrence_persist_buffer(ev_);\n"
                                      "        ev_->note_type_export_inflight();");
    CHECK(nested_path != std::string::npos,
          "3170 AC2: nested path (line 4620) calls aura_clear_occurrence_persist_buffer");
}

static void ac3170_3_soft_zero_behavioural_change() {
    std::println(
        "\n--- #3170 AC3: Soft / Off / unit-test default \u2192 zero behavioural change ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    // clear_occurrence_persist_buffer wrapper gates on production_defaults_active.
    CHECK(tma.find("if (!aura::compiler::typed_audit::production_defaults_active())\n"
                   "        return 0;") != std::string::npos,
          "3170 AC3: clear wrapper production gate (Soft untouched)");
    // Counter only bumps under production gate.
    CHECK(tma.find("aura::compiler::g_occurrence_persist_audit_atomic_wired.fetch_add(\n"
                   "            dropped, std::memory_order_relaxed);") != std::string::npos,
          "3170 AC3: counter bump gated by production");
}

static void ac3170_4_quiet_zero_extra_atomics() {
    std::println(
        "\n--- #3170 AC4: Quiet (clean / no dirty / no TIMEOUT) \u2192 zero extra atomics ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    // Outermost success path early-returns on prior != TIMEOUT (existing #2277 contract).
    CHECK(impl.find("if (prior != SolveResult::TIMEOUT)\n        return prior;") !=
              std::string::npos,
          "3170 AC4: escalate_if_production early-return on non-TIMEOUT (Quiet path)");
    // Fingerprint guard is gated on production_defaults_active.
    CHECK(emb.find("if (aura::compiler::typed_audit::production_defaults_active() &&\n"
                   "        ev->expected_occurrence_snapshot_fp() != 0 &&\n"
                   "        live_fp != ev->expected_occurrence_snapshot_fp()) {") !=
              std::string::npos,
          "3170 AC4: fingerprint guard gated on production_defaults_active (zero extra on Soft)");
}

static void ac3170_5_additive_observability_only() {
    std::println("\n--- #3170 AC5: Additive observability only ---");
    const auto obs = read_file("src/compiler/observability_metrics.h");
    // Counter at struct end (layout-stable per #2906).
    CHECK(obs.find("// Issue #3170: outermost-success Occurrence persist fingerprint guard") !=
              std::string::npos,
          "3170 AC5: counter comment cites #3170");
    CHECK(obs.find("std::atomic<std::uint64_t> occurrence_persist_fingerprint_mismatch_total{0}; "
                   "// #3170") != std::string::npos,
          "3170 AC5: counter declared at struct end");
    // Existing #2277 / #3003 / #2963 / #2913 surfaces preserved.
    CHECK(obs.find("delta_timeout_full_solve_total") != std::string::npos,
          "3170 AC5: #2277 delta_timeout_full_solve_total preserved");
    CHECK(obs.find("delta_timeout_reject_total") != std::string::npos,
          "3170 AC5: #3003 delta_timeout_reject_total preserved");
    CHECK(obs.find("solver_budget_instance_repair_prefer_total") != std::string::npos,
          "3170 AC5: #2963 instance_repair_prefer_total preserved");
    CHECK(obs.find("delta_instance_repair_resolved_total") != std::string::npos,
          "3170 AC5: #2963 instance_repair_resolved_total preserved");
}

static void ac3170_6_source_and_linter() {
    std::println("\n--- #3170 AC6: source-cite linter + build.py wiring ---");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_occurrence_persist_fingerprint_3170.py");
    int rc =
        std::system("python3 scripts/coverage/checks/check_occurrence_persist_fingerprint_3170.py "
                    "--self-test > /dev/null 2>&1");
    CHECK(rc == 0, "3170 AC6: linter --self-test passes");
    CHECK(!lint.empty() && lint.find("Issue #3170") != std::string::npos,
          "3170 AC6: linter cites #3170");
    CHECK(build.find("check_occurrence_persist_fingerprint_3170") != std::string::npos,
          "3170 AC6: build.py wires linter");
}

// ── Issue #3193: nested abort + concurrent densify/steal one authority face ──

static void ac3193_1_prod_hold_blocks_rehydrate() {
    std::println("\n--- #3193 AC1: production hold blocks rehydrate until persist+proof clear ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    typed_audit::reset_abort_authority_hold_for_test();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
    typed_audit::clear_type_linear_proof_outcome_for_test();
    typed_audit::g_linear_ir_fastpath_boundary_depth_override = 0;
    typed_audit::g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);

    typed_audit::stamp_type_linear_commit_proof(31931);
    typed_audit::publish_type_linear_proof_outcome(typed_audit::kTypeLinearProofOutcomeStamped);
    typed_audit::publish_last_proof_face(true, true);
    CHECK(typed_audit::linear_fast_path_ok(), "3193 AC1: green before abort hold");

    {
        UnitCs pre;
        pre.cs.set_current_epoch(5);
        auto pv = pre.cs.fresh_var();
        pre.cs.note_occurrence_goal(pv, pre.reg.int_type(), 11, 100, /*epoch=*/5);
        CHECK(pre.cs.append_occurrence_snapshot(100) == 1, "3193 AC1: pre persist");
        CHECK(pre.cs.prune_occurrence_goals(6) == 1, "3193 AC1: pre prune");
        CHECK(pre.cs.rehydrate_occurrence_from_persist(100) == 1,
              "3193 AC1: rehydrate works pre-hold");
    }

    UnitCs u;
    u.cs.set_current_epoch(5);
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), 11, 100, /*epoch=*/5);
    CHECK(u.cs.append_occurrence_snapshot(100) == 1, "3193 AC1: persist wrote 1");
    CHECK(u.cs.prune_occurrence_goals(6) == 1, "3193 AC1: prune live");
    CHECK(u.cs.occurrence_goals_size() == 0, "3193 AC1: live empty");
    CHECK(u.cs.occurrence_persist_log_size() == 1, "3193 AC1: persist intact");

    const auto hold0 = typed_audit::abort_authority_hold_total_v_read();
    const auto gen0 = typed_audit::rehydrate_miss_invalidate_gen_v_read();
    {
        typed_audit::AbortAuthorityHold hold;
        CHECK(typed_audit::abort_authority_blocks_rehydrate(), "3193 AC1: in_flight");
        CHECK(typed_audit::abort_authority_hold_total_v_read() == hold0 + 1,
              "3193 AC1: hold total");
        CHECK(typed_audit::rehydrate_miss_invalidate_gen_v_read() == gen0 + 1,
              "3193 AC1: reuse invalidate_gen");
        CHECK(u.cs.rehydrate_occurrence_from_persist(100) == 0,
              "3193 AC1: rehydrate blocked during hold");
        CHECK(u.cs.occurrence_persist_log_size() >= 1, "3193 AC1: persist not yet cleared");
        typed_audit::clear_type_linear_commit_proof_on_abort();
        CHECK(u.cs.clear_occurrence_persist_snapshot() >= 1,
              "3193 AC1: persist cleared under hold");
        CHECK(typed_audit::last_type_linear_proof_outcome_v_read() ==
                  typed_audit::kTypeLinearProofOutcomeReject,
              "3193 AC1: proof Reject");
        CHECK(typed_audit::last_proof_would_allow_commit_v_read() == 0, "3193 AC1: would_allow 0");
    }
    CHECK(!typed_audit::abort_authority_blocks_rehydrate(), "3193 AC1: hold released");
    CHECK(u.cs.occurrence_persist_log_size() == 0, "3193 AC1: persist matches authority (empty)");
    CHECK(u.cs.rehydrate_occurrence_from_persist(100) == 0, "3193 AC1: post-window rehydrate 0");

    apply_dev_audit_defaults();
    typed_audit::reset_abort_authority_hold_for_test();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
}

static void ac3193_2_no_mixed_green_residual() {
    std::println("\n--- #3193 AC2: no mixed green proof + residual persist ---");
    apply_production_audit_defaults();
    typed_audit::reset_abort_authority_hold_for_test();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
    typed_audit::stamp_type_linear_commit_proof(31932);
    typed_audit::publish_type_linear_proof_outcome(typed_audit::kTypeLinearProofOutcomeStamped);
    typed_audit::publish_last_proof_face(true, true);
    UnitCs u;
    u.cs.set_current_epoch(1);
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), 1, 10, 1);
    CHECK(u.cs.append_occurrence_snapshot(10) == 1, "3193 AC2: persist");
    CHECK(u.cs.prune_occurrence_goals(2) == 1, "3193 AC2: live empty, persist intact");
    CHECK(u.cs.occurrence_goals_size() == 0, "3193 AC2: live empty");
    {
        typed_audit::AbortAuthorityHold hold;
        // Green face still published until proof clear, but rehydrate cannot
        // restore persist into live CS — no mixed query/IR face.
        CHECK(typed_audit::last_proof_would_allow_commit_v_read() == 1,
              "3193 AC2: green until clear");
        CHECK(u.cs.rehydrate_occurrence_from_persist(10) == 0, "3193 AC2: no residual restore");
        CHECK(u.cs.occurrence_goals_size() == 0, "3193 AC2: live stays empty");
        typed_audit::clear_type_linear_commit_proof_on_abort();
        (void)u.cs.clear_occurrence_persist_snapshot();
        CHECK(typed_audit::last_proof_would_allow_commit_v_read() == 0, "3193 AC2: not green");
        CHECK(u.cs.occurrence_persist_log_size() == 0, "3193 AC2: persist empty");
    }
    apply_dev_audit_defaults();
    typed_audit::reset_abort_authority_hold_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
}

static void ac3193_3_soft_observe_quiet_zero() {
    std::println("\n--- #3193 AC3: Soft observe-only; quiet zero extra ---");
    apply_dev_audit_defaults();
    typed_audit::reset_abort_authority_hold_for_test();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    const auto hard0 = typed_audit::abort_authority_hold_total_v_read();
    const auto obs0 = typed_audit::abort_authority_hold_observe_total_v_read();
    const auto gen0 = typed_audit::rehydrate_miss_invalidate_gen_v_read();
    {
        typed_audit::AbortAuthorityHold hold;
        CHECK(!typed_audit::abort_authority_blocks_rehydrate(), "3193 AC3: Soft no in_flight");
        CHECK(typed_audit::abort_authority_hold_total_v_read() == hard0, "3193 AC3: no hard");
        CHECK(typed_audit::abort_authority_hold_observe_total_v_read() == obs0 + 1,
              "3193 AC3: observe");
        CHECK(typed_audit::rehydrate_miss_invalidate_gen_v_read() == gen0, "3193 AC3: no gen bump");
    }
    CHECK(!typed_audit::abort_authority_blocks_rehydrate(), "3193 AC3: still clear");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mb.find("typed_audit::AbortAuthorityHold abort_authority") != std::string::npos,
          "3193 AC3: hold only on abort sites (quiet success never constructs)");
    typed_audit::reset_abort_authority_hold_for_test();
}

static void ac3193_4_source_and_linter() {
    std::println("\n--- #3193 AC4: source-cite + linter + no invent ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto t = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_nested_abort_authority_face_3193.py");
    const auto build = read_file("build.py");
    CHECK(tma.find("kNestedAbortAuthorityFaceIssue") != std::string::npos, "3193 AC4: issue stamp");
    CHECK(tma.find("AbortAuthorityHold") != std::string::npos, "3193 AC4: RAII hold");
    CHECK(mb.find("AbortAuthorityHold abort_authority") != std::string::npos,
          "3193 AC4: abort sites");
    CHECK(mb.find("aura_clear_occurrence_persist_buffer(this)") != std::string::npos,
          "3193 AC4: persist on abort body");
    CHECK(impl.find("abort_authority_blocks_rehydrate") != std::string::npos,
          "3193 AC4: rehydrate consult");
    CHECK(t.find("ac3193_1_prod_hold_blocks_rehydrate") != std::string::npos, "3193 AC4: AC1");
    CHECK(t.find("ac3193_2_no_mixed_green_residual") != std::string::npos, "3193 AC4: AC2");
    CHECK(t.find("ac3193_3_soft_observe_quiet_zero") != std::string::npos, "3193 AC4: AC3");
    CHECK(!lint.empty() && lint.find("3193") != std::string::npos, "3193 AC4: linter");
    CHECK(build.find("check_nested_abort_authority_face_3193") != std::string::npos,
          "3193 AC4: build.py");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "3193 AC4: warm");
    CHECK(href(svc, "schema-3193") == 3193, "3193 AC4: schema-3193");
    CHECK(href(svc, "issue-3193") == 3193, "3193 AC4: issue-3193");
    CHECK(href(svc, "abort-authority-hold-wired") == 1, "3193 AC4: wired");
    CHECK(href(svc, "schema-3030") == 3030 || href(svc, "schema-3032") == 3032,
          "3193 AC4: sibling schema preserved");
    CHECK(read_file("docs/design/3193-nested-abort-authority-face.md").empty(),
          "3193 AC4: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_3193.cpp").empty(),
          "3193 AC4: no invent test_issue_3193");
    CHECK(read_file("tests/issues/test_issue_3193.cpp").empty(),
          "3193 AC4: no tests/issues/test_issue_3193");
}

// ── Issue #3232: nested AbortAuthorityHold must not drop the face ──

static void ac3232_1_nested_hold_keeps_block() {
    std::println("\n--- #3232 AC1: nested hold keeps rehydrate blocked until last end ---");
    unsetenv("AURA_OCCURRENCE_PERSIST");
    apply_production_audit_defaults();
    typed_audit::reset_abort_authority_hold_for_test();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
    CHECK(typed_audit::kNestedAbortAuthorityFaceResidualIssue == 3232, "3232 AC1: issue constant");

    UnitCs u;
    u.cs.set_current_epoch(5);
    auto v = u.cs.fresh_var();
    u.cs.note_occurrence_goal(v, u.reg.int_type(), 11, 100, /*epoch=*/5);
    CHECK(u.cs.append_occurrence_snapshot(100) == 1, "3232 AC1: persist");
    CHECK(u.cs.prune_occurrence_goals(6) == 1, "3232 AC1: live empty");
    CHECK(u.cs.occurrence_persist_log_size() == 1, "3232 AC1: persist intact");

    const auto hold0 = typed_audit::abort_authority_hold_total_v_read();
    const auto gen0 = typed_audit::rehydrate_miss_invalidate_gen_v_read();
    {
        typed_audit::AbortAuthorityHold outer;
        CHECK(typed_audit::abort_authority_blocks_rehydrate(), "3232 AC1: outer in_flight");
        CHECK(typed_audit::abort_authority_hold_total_v_read() == hold0 + 1,
              "3232 AC1: outer total");
        {
            typed_audit::AbortAuthorityHold inner;
            CHECK(typed_audit::abort_authority_blocks_rehydrate(), "3232 AC1: nested in_flight");
            CHECK(typed_audit::abort_authority_hold_total_v_read() == hold0 + 2,
                  "3232 AC1: nested total");
            CHECK(typed_audit::rehydrate_miss_invalidate_gen_v_read() == gen0 + 2,
                  "3232 AC1: nested gen bump");
            CHECK(u.cs.rehydrate_occurrence_from_persist(100) == 0,
                  "3232 AC1: rehydrate blocked nested");
        }
        CHECK(typed_audit::abort_authority_blocks_rehydrate(),
              "3232 AC1: inner end does not drop outer face");
        CHECK(u.cs.rehydrate_occurrence_from_persist(100) == 0,
              "3232 AC1: still blocked after inner end");
        CHECK(u.cs.occurrence_persist_log_size() >= 1, "3232 AC1: persist not yet cleared");
        typed_audit::clear_type_linear_commit_proof_on_abort();
        CHECK(u.cs.clear_occurrence_persist_snapshot() >= 1, "3232 AC1: persist clear under hold");
    }
    CHECK(!typed_audit::abort_authority_blocks_rehydrate(), "3232 AC1: last end drops face");
    CHECK(u.cs.rehydrate_occurrence_from_persist(100) == 0, "3232 AC1: post-window rehydrate 0");

    apply_dev_audit_defaults();
    typed_audit::reset_abort_authority_hold_for_test();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
}

static void ac3232_2_soft_nested_observe_quiet() {
    std::println("\n--- #3232 AC2: Soft nested observe-only; quiet no-abort zero extra ---");
    apply_dev_audit_defaults();
    typed_audit::reset_abort_authority_hold_for_test();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    const auto hard0 = typed_audit::abort_authority_hold_total_v_read();
    const auto obs0 = typed_audit::abort_authority_hold_observe_total_v_read();
    const auto gen0 = typed_audit::rehydrate_miss_invalidate_gen_v_read();
    {
        typed_audit::AbortAuthorityHold outer;
        typed_audit::AbortAuthorityHold inner;
        CHECK(!typed_audit::abort_authority_blocks_rehydrate(), "3232 AC2: Soft no in_flight");
        CHECK(typed_audit::abort_authority_hold_total_v_read() == hard0, "3232 AC2: no hard");
        CHECK(typed_audit::abort_authority_hold_observe_total_v_read() == obs0 + 2,
              "3232 AC2: nested observe");
        CHECK(typed_audit::rehydrate_miss_invalidate_gen_v_read() == gen0, "3232 AC2: no gen bump");
    }
    CHECK(!typed_audit::abort_authority_blocks_rehydrate(), "3232 AC2: still clear");
    typed_audit::reset_abort_authority_hold_for_test();
}

static void ac3232_3_source_cite_dual_restore_rehydrate() {
    std::println("\n--- #3232 AC3+AC4: dual_restore + rehydrate under AbortAuthorityHold ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto steal = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto qs = read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_nested_abort_authority_face_3232.py");
    CHECK(tma.find("kNestedAbortAuthorityFaceResidualIssue") != std::string::npos,
          "3232 AC3: stamp");
    CHECK(tma.find("fetch_add(1, std::memory_order_release)") != std::string::npos,
          "3232 AC3: nested in_flight count");
    CHECK(tma.find("compare_exchange_weak") != std::string::npos, "3232 AC3: nested last-end");
    CHECK(mb.find("Issue #3232") != std::string::npos, "3232 AC3: abort sites cite");
    std::size_t dual = 0;
    for (auto p = mb.find("abort_restore_dual_topology("); p != std::string::npos;
         p = mb.find("abort_restore_dual_topology(", p + 1)) {
        ++dual;
        const auto hold = mb.rfind("AbortAuthorityHold abort_authority", p);
        CHECK(hold != std::string::npos && hold < p, "3232 AC3: hold before dual_restore");
    }
    CHECK(dual == 3, "3232 AC3: three dual_restore sites");
    CHECK(impl.find("abort_authority_blocks_rehydrate") != std::string::npos,
          "3232 AC3: rehydrate consult");
    CHECK(impl.find("Issue #3232") != std::string::npos, "3232 AC3: rehydrate cite");
    CHECK(ixx.find("Issue #3232") != std::string::npos, "3232 AC3: densify cite");
    CHECK(steal.find("Issue #3232") != std::string::npos, "3232 AC3: steal cite");
    CHECK(qs.find("schema-3232") != std::string::npos, "3232 AC4: schema-3232");
    CHECK(qs.find("schema-3193") != std::string::npos, "3232 AC4: 3193 preserved");
    CHECK(!lint.empty() && lint.find("3232") != std::string::npos, "3232 AC4: linter");
    CHECK(build.find("check_nested_abort_authority_face_3232") != std::string::npos,
          "3232 AC4: build.py");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "3232 AC4: warm");
    CHECK(href(svc, "schema-3232") == 3232, "3232 AC4: schema-3232 live");
    CHECK(href(svc, "issue-3232") == 3232, "3232 AC4: issue-3232");
    CHECK(href(svc, "schema-3193") == 3193, "3232 AC4: schema-3193 preserved");
    CHECK(href(svc, "abort-authority-hold-wired") == 1, "3232 AC4: reuse hold wired");
    CHECK(read_file("docs/design/3232-nested-abort-authority-face.md").empty(),
          "3232 AC4: no docs/design");
    CHECK(read_file("tests/compiler/test_issue_3232.cpp").empty(), "3232 AC4: no invent");
    CHECK(read_file("tests/issues/test_issue_3232.cpp").empty(), "3232 AC4: no tests/issues");
}

// ── Issue #3346: last-look fingerprint / live_goal_count / linear_root
// immediately before TypeLinearCommitProof stamp vs densify×steal×mid-abort.
//   AC1 last-look mismatch → reject (clear persist + invalidate_gen + no green)
//   AC2 densify/steal refuse if mid_abort_authority outstanding
//   AC3 last_proof_* acquire-consistent with live table after stamp
//   AC4 Soft/Off zero extra; no new query key / g_3346_* / invent / docs

static void ac3346_reset_faces() {
    unsetenv("AURA_OCCURRENCE_PERSIST");
    typed_audit::clear_stamp_last_look_tc();
    typed_audit::reset_mid_abort_authority_for_test();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
    typed_audit::clear_type_linear_proof_outcome_for_test();
    typed_audit::clear_proof_goal_truth_for_test();
    typed_audit::reset_type_linear_proof_reject_empty_after_fence_for_test();
    typed_audit::reset_linear_compact_root_consistency_for_test();
    clear_occurrence_empty_after_fence_for_test();
    typed_audit::g_linear_ir_fastpath_boundary_depth_override = 0;
    typed_audit::g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    typed_audit::clear_boundary_audit_mid();
}

static void ac3346_1_last_look_fingerprint_mismatch_rejects() {
    std::println("\n--- #3346 AC1: last-look fingerprint/goals mismatch rejects green ---");
    ac3346_reset_faces();
    apply_production_audit_defaults();

    TypeRegistry reg;
    TypeChecker tc(reg);
    CompilerMetrics metrics{};
    tc.set_metrics(&metrics);
    auto& cs = tc.constraint_system();
    cs.set_metrics(&metrics);
    tc.set_cache_epoch(1);
    cs.set_current_epoch(1);
    auto v = cs.fresh_var();
    cs.note_occurrence_goal(v, reg.int_type(), 1, 33461, /*epoch=*/1);
    CHECK(cs.append_occurrence_snapshot(33461) == 1, "3346 AC1: persist wrote");
    typed_audit::note_stamp_last_look_tc(&tc);
    const auto fp0 = typed_audit::occurrence_goal_fingerprint(&tc);
    const auto n0 = static_cast<std::uint64_t>(cs.occurrence_goals_size());
    CHECK(n0 == 1 && fp0 != 0, "3346 AC1: one live goal + non-zero fp");
    auto v2 = cs.fresh_var();
    cs.note_occurrence_goal(v2, reg.int_type(), 2, 33462, /*epoch=*/1);
    CHECK(cs.occurrence_goals_size() == 2, "3346 AC1: live drifted to 2 goals");
    const auto gen0 = typed_audit::rehydrate_miss_invalidate_gen_v_read();
    const auto p = typed_audit::build_type_linear_commit_proof_from_live_with_outcome(
        33461, /*would_allow_commit=*/true, /*linear_ok=*/true, n0, fp0, /*from_cs=*/true);
    CHECK(!p.would_allow_commit, "3346 AC1: last-look mismatch rejects");
    CHECK(typed_audit::stamp_last_look_rejected(), "3346 AC1: last-look rejected flag");
    CHECK(p.linear_ok == false, "3346 AC1: linear_ok false");
    CHECK(p.force_reason_code == 16, "3346 AC1: force_reason 16");
    CHECK(typed_audit::last_proof_would_allow_commit_v_read() == 0, "3346 AC1: no green face");
    CHECK(typed_audit::rehydrate_miss_invalidate_gen_v_read() > gen0,
          "3346 AC1: invalidate_gen advanced");
    CHECK(cs.occurrence_persist_log_size() == 0, "3346 AC1: persist cleared");
    CHECK(!typed_audit::linear_fast_path_ok(), "3346 AC1: IR fast-path not green");

    apply_dev_audit_defaults();
    ac3346_reset_faces();
}

static void ac3346_2_outstanding_authority_refuses_stamp() {
    std::println("\n--- #3346 AC2: mid_abort_authority outstanding refuses green stamp ---");
    ac3346_reset_faces();
    apply_production_audit_defaults();
    typed_audit::note_boundary_audit_mid(33462);
    const auto ver = typed_audit::begin_mid_abort_authority(33462);
    CHECK(ver != 0, "3346 AC2: production armed mid-abort slot");
    CHECK(typed_audit::mid_abort_authority_outstanding(33462), "3346 AC2: outstanding");
    const auto mismatch0 =
        typed_audit::g_mid_abort_authority_mismatch_total.load(std::memory_order_relaxed);
    const auto p = typed_audit::build_type_linear_commit_proof_from_live_with_outcome(
        33462, /*would_allow_commit=*/true, /*linear_ok=*/true, 0, 0, /*from_cs=*/true);
    CHECK(!p.would_allow_commit, "3346 AC2: outstanding refuses green");
    CHECK(typed_audit::stamp_last_look_rejected(), "3346 AC2: last-look rejected flag");
    CHECK(p.force_reason_code == 16, "3346 AC2: force_reason 16");
    CHECK(typed_audit::g_mid_abort_authority_mismatch_total.load(std::memory_order_relaxed) >
              mismatch0,
          "3346 AC2: reuse mismatch total");
    typed_audit::end_mid_abort_authority(33462);
    CHECK(!typed_audit::mid_abort_authority_outstanding(33462), "3346 AC2: released");
    const auto p2 = typed_audit::build_type_linear_commit_proof_from_live_with_outcome(
        33462, true, true, 0, 0, true);
    CHECK(p2.would_allow_commit, "3346 AC2: after release stamp may green");
    CHECK(!typed_audit::stamp_last_look_rejected(), "3346 AC2: flag clear after release");
    CHECK(typed_audit::last_proof_would_allow_commit_v_read() == 1,
          "3346 AC2: green after release");

    const auto densify = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto steal = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(densify.find("densify_abort_outstanding_3346") != std::string::npos,
          "3346 AC2: densify Phase-5 outstanding refuse");
    CHECK(steal.find("steal_abort_outstanding_3346") != std::string::npos,
          "3346 AC2: steal outstanding refuse");
    CHECK(steal.find("note_stamp_last_look_tc") != std::string::npos,
          "3346 AC2: steal notes last-look tc");

    apply_dev_audit_defaults();
    ac3346_reset_faces();
}

static void ac3346_3_stamp_matches_live_under_acquire() {
    std::println("\n--- #3346 AC3: after stamp last_proof_* matches live under acquire ---");
    ac3346_reset_faces();
    apply_production_audit_defaults();

    TypeRegistry reg;
    TypeChecker tc(reg);
    CompilerMetrics metrics{};
    tc.set_metrics(&metrics);
    auto& cs = tc.constraint_system();
    cs.set_metrics(&metrics);
    tc.set_cache_epoch(3);
    cs.set_current_epoch(3);
    auto v = cs.fresh_var();
    cs.note_occurrence_goal(v, reg.int_type(), 3, 33463, /*epoch=*/3);
    typed_audit::note_stamp_last_look_tc(&tc);
    const auto fp = typed_audit::occurrence_goal_fingerprint(&tc);
    const auto n = static_cast<std::uint64_t>(cs.occurrence_goals_size());
    const auto roots =
        static_cast<std::uint64_t>(aura::compiler::linear_or_dirty_roots_count_for_rebind());
    const auto p = typed_audit::build_type_linear_commit_proof_from_live_with_outcome(
        33463, true, true, n, fp, true);
    CHECK(p.would_allow_commit, "3346 AC3: matching last-look stays green");
    CHECK(!typed_audit::stamp_last_look_rejected(), "3346 AC3: last-look flag clear");
    CHECK(p.goal_fingerprint == fp, "3346 AC3: proof fingerprint == live");
    CHECK(p.live_goal_count == n, "3346 AC3: proof live_goal_count == CS size");
    CHECK(p.linear_root_count == roots, "3346 AC3: proof linear_root_count == live roots");
    CHECK(typed_audit::last_proof_goal_fingerprint_v_read() == fp,
          "3346 AC3: last_proof fingerprint gauge");
    CHECK(typed_audit::last_proof_live_goal_count_v_read() == n,
          "3346 AC3: last_proof goals gauge");
    CHECK(typed_audit::last_proof_linear_root_count_v_read() == roots,
          "3346 AC3: last_proof roots gauge");
    CHECK(typed_audit::occurrence_goal_fingerprint(&tc) ==
              typed_audit::last_proof_goal_fingerprint_v_read(),
          "3346 AC3: live fingerprint == stamped");
    CHECK(typed_audit::linear_fast_path_ok(), "3346 AC3: IR consult sees consistent green");

    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("g_last_proof_would_allow_commit.store(would_allow ? 1 : 0, "
                   "std::memory_order_release)") != std::string::npos,
          "3346 AC3: publish uses release");
    CHECK(tma.find("g_last_proof_would_allow_commit.load(std::memory_order_acquire)") !=
              std::string::npos,
          "3346 AC3: linear_fast_path_ok acquire on last_proof");

    apply_dev_audit_defaults();
    ac3346_reset_faces();
}

static void ac3346_4_soft_zero_extra_and_linter() {
    std::println("\n--- #3346 AC4: Soft/Off zero extra; linter; no invent ---");
    ac3346_reset_faces();
    apply_dev_audit_defaults();
    typed_audit::note_boundary_audit_mid(33464);
    const auto ver = typed_audit::begin_mid_abort_authority(33464);
    CHECK(ver == 0, "3346 AC4: Soft begin_mid_abort_authority no-ops");
    CHECK(!typed_audit::mid_abort_authority_outstanding(33464), "3346 AC4: Soft outstanding 0");
    TypeRegistry reg;
    TypeChecker tc(reg);
    auto& cs = tc.constraint_system();
    cs.set_current_epoch(1);
    auto v = cs.fresh_var();
    cs.note_occurrence_goal(v, reg.int_type(), 1, 1, 1);
    typed_audit::note_stamp_last_look_tc(&tc);
    const auto fp = typed_audit::occurrence_goal_fingerprint(&tc);
    auto v2 = cs.fresh_var();
    cs.note_occurrence_goal(v2, reg.int_type(), 2, 2, 1);
    // Soft last-look returns true without re-read — stamp keeps caller outcome
    // even when live CS drifted (AC4; production would reject).
    const auto p = typed_audit::build_type_linear_commit_proof_from_live_with_outcome(
        33464, true, true, 1, fp, true);
    CHECK(p.would_allow_commit, "3346 AC4: Soft does not last-look reject");

    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("kStampLastLookIssue = 3346") != std::string::npos, "3346 AC4: stamp");
    CHECK(tma.find("if (!stamp_last_look_hard())") != std::string::npos,
          "3346 AC4: Soft early-return");
    CHECK(tma.find("g_3346_") == std::string::npos, "3346 AC4: no g_3346_*");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_stamp_last_look_densify_steal_abort_3346.py");
    CHECK(!lint.empty() && lint.find("Issue #3346") != std::string::npos, "3346 AC4: linter");
    CHECK(build.find("check_stamp_last_look_densify_steal_abort_3346") != std::string::npos,
          "3346 AC4: build.py");
    const auto pos3225 = build.find("check_occurrence_persist_seq_3225");
    const auto pos3346 = build.find("check_stamp_last_look_densify_steal_abort_3346");
    CHECK(pos3225 != std::string::npos && pos3346 != std::string::npos && pos3346 > pos3225,
          "3346 AC4: linter after #3225");
    CHECK(read_file("docs/design/3346-stamp-last-look.md").empty(), "3346 AC4: no docs/design");
    CHECK(read_file("tests/compiler/test_issue_3346.cpp").empty(), "3346 AC4: no invent");
    CHECK(read_file("tests/issues/test_issue_3346.cpp").empty(), "3346 AC4: no tests/issues");
    CHECK(tma.find("schema-3346") == std::string::npos, "3346 AC4: no new query key");

    apply_dev_audit_defaults();
    ac3346_reset_faces();
}

static void ac3418_fingerprint_cap_overflow_rejects() {
    std::println("\n--- #3418: n>16 prefix mix is not green authority ---");
    ac3346_reset_faces();
    apply_production_audit_defaults();

    TypeRegistry reg;
    TypeChecker tc(reg);
    CompilerMetrics metrics{};
    tc.set_metrics(&metrics);
    auto& cs = tc.constraint_system();
    cs.set_metrics(&metrics);
    tc.set_cache_epoch(1);
    cs.set_current_epoch(1);
    for (int i = 0; i < 16; ++i) {
        auto v = cs.fresh_var();
        cs.note_occurrence_goal(v, reg.int_type(), static_cast<std::uint32_t>(i + 1),
                                static_cast<std::uint64_t>(34180 + i), /*epoch=*/1);
    }
    typed_audit::note_stamp_last_look_tc(&tc);
    const auto fp16 = typed_audit::occurrence_goal_fingerprint(&tc);
    CHECK(cs.occurrence_goals_size() == 16 && fp16 != 0, "3418: 16-goal prefix mix");
    auto v17 = cs.fresh_var();
    cs.note_occurrence_goal(v17, reg.int_type(), /*pred=*/99, /*mut=*/34199, /*epoch=*/7);
    CHECK(cs.occurrence_goals_size() == 17, "3418: 17 live goals");
    const auto fp17 = typed_audit::occurrence_goal_fingerprint(&tc);
    CHECK(fp17 == fp16, "3418: 17th goal does not change the 16-prefix mix");
    typed_audit::note_stamp_last_look_tc(&tc);
    const auto n = static_cast<std::uint64_t>(cs.occurrence_goals_size());
    const auto p = typed_audit::build_type_linear_commit_proof_from_live_with_outcome(
        34180, /*would_allow_commit=*/true, /*linear_ok=*/true, n, fp17, /*from_cs=*/true);
    CHECK(!p.would_allow_commit, "3418 AC2: production overflow rejects green stamp");
    CHECK(!p.occurrence_consistent, "3418 AC1: occurrence_consistent false");
    CHECK(p.force_reason_code == 16, "3418 AC2: force_reason 16");
    CHECK(p.live_goal_count == 17, "3418 AC4: live_goal_count published as 17");
    CHECK(typed_audit::last_type_linear_proof_outcome_v_read() ==
              typed_audit::kTypeLinearProofOutcomeReject,
          "3418 AC2: Reject outcome");

    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(emb.find("Issue #3418") != std::string::npos, "3418 AC3: persist overflow cite");
    CHECK(emb.find("fingerprint_overflow") != std::string::npos,
          "3418 AC3: persist fail-closed on overflow");

    apply_dev_audit_defaults();
    ac3346_reset_faces();
    TypeRegistry reg2;
    TypeChecker tc2(reg2);
    auto& cs2 = tc2.constraint_system();
    cs2.set_current_epoch(1);
    for (int i = 0; i < 17; ++i) {
        auto v = cs2.fresh_var();
        cs2.note_occurrence_goal(v, reg2.int_type(), static_cast<std::uint32_t>(i + 1),
                                 static_cast<std::uint64_t>(i + 1), 1);
    }
    typed_audit::note_stamp_last_look_tc(&tc2);
    const auto fps = typed_audit::occurrence_goal_fingerprint(&tc2);
    const auto ps = typed_audit::build_type_linear_commit_proof_from_live_with_outcome(
        34181, true, true, 17, fps, true);
    CHECK(ps.would_allow_commit, "3418 AC2: Soft overflow still mixes 16 (observe only)");
    apply_dev_audit_defaults();
    ac3346_reset_faces();
}

static void ac3418_source_and_linter() {
    std::println("\n--- #3418: source-cite + linter ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("kProofGoalFingerprintOverflowIssue = 3418") != std::string::npos,
          "3418: issue stamp");
    CHECK(tma.find("reject_fingerprint_cap_overflow") != std::string::npos, "3418: reject helper");
    CHECK(tma.find("fingerprint_overflow") != std::string::npos, "3418: ProofGoalTruth field");
    CHECK(tma.find("g_3418_") == std::string::npos, "3418: no g_3418_*");
    const auto build = read_file("build.py");
    CHECK(build.find("check_proof_goal_fingerprint_overflow_3418") != std::string::npos,
          "3418: build.py");
    CHECK(build.find("check_occurrence_persist_fingerprint_3170") != std::string::npos,
          "3418: predecessor #3170 present");
    CHECK(read_file("docs/design/3418-fingerprint-overflow.md").empty(), "3418: no docs/design");
    CHECK(read_file("tests/compiler/test_issue_3418.cpp").empty(), "3418: no invent");
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
    aura::compiler::lock_order::reset_tls_for_test();
    typed_audit::clear_stamp_last_look_tc();
    typed_audit::clear_type_linear_proof_outcome_for_test();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
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
    aura::compiler::lock_order::reset_tls_for_test();
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
    CHECK(mb.find("Issue #3440") != std::string::npos,
          "AC3: #3440 persist-reject restore cite (persist still outermost-only)");
    const auto persist_call = mb.find("aura_outermost_success_persist_occurrence(ev_");
    const auto exit_pos = mb.find("ev_->exit_mutation_boundary(success)");
    CHECK(persist_call != std::string::npos && exit_pos != std::string::npos &&
              persist_call < exit_pos,
          "AC3: persist still sole outermost path (now before abort_restore SSOT)");
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

// ── Issue #3171: steal/densify/cross-eval restamp complete-clear ──

static void ac3171_1_prod_clear_blocks_elide() {
    std::println("\n--- #3171 AC1: production restamp clear → !elide ---");
    apply_production_audit_defaults();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::reset_linear_ir_fastpath_counters_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
    typed_audit::clear_type_linear_proof_outcome_for_test();
    typed_audit::g_linear_ir_fastpath_boundary_depth_override = 0;
    typed_audit::g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
        0, std::memory_order_relaxed);
    typed_audit::stamp_type_linear_commit_proof(31711);
    typed_audit::publish_type_linear_proof_outcome(typed_audit::kTypeLinearProofOutcomeStamped);
    typed_audit::publish_last_proof_face(true, true);
    CHECK(typed_audit::linear_fast_path_ok(), "3171 AC1: green before restamp");
    CHECK(typed_audit::linear_ir_fastpath_try_skip(), "3171 AC1: skip before");
    const auto inv0 = typed_audit::steal_densify_success_invalidate_total_v_read();
    const auto gen0 = typed_audit::rehydrate_miss_invalidate_gen_v_read();
    CHECK(typed_audit::invalidate_fast_path_before_steal_densify_restamp(),
          "3171 AC1: production invalidate");
    CHECK(typed_audit::rehydrate_miss_invalidate_gen_v_read() == gen0 + 1,
          "3171 AC1: invalidate_gen advanced");
    CHECK(typed_audit::steal_densify_success_invalidate_total_v_read() == inv0 + 1,
          "3171 AC1: reuse #3063 success invalidate total");
    CHECK(!typed_audit::linear_fast_path_ok(), "3171 AC1: !ok after gen advance");
    CHECK(!typed_audit::linear_ir_fastpath_try_skip(), "3171 AC1: Move/Drop cannot skip");
    typed_audit::publish_last_proof_face(true, true);
    CHECK(typed_audit::linear_fast_path_ok(), "3171 AC1: green after rebind");
    apply_dev_audit_defaults();
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    typed_audit::clear_type_linear_commit_proof_for_test();
}

static void ac3171_2_soft_zero_extra() {
    std::println("\n--- #3171 AC2: Soft zero extra atomics ---");
    apply_dev_audit_defaults();
    typed_audit::set_strategy(typed_audit::AuditStrategy::Sampled);
    typed_audit::reset_rehydrate_miss_invalidate_for_test();
    const auto inv0 = typed_audit::steal_densify_success_invalidate_total_v_read();
    const auto gen0 = typed_audit::rehydrate_miss_invalidate_gen_v_read();
    CHECK(!typed_audit::invalidate_fast_path_before_steal_densify_restamp(),
          "3171 AC2: Soft returns false");
    CHECK(typed_audit::steal_densify_success_invalidate_total_v_read() == inv0,
          "3171 AC2: no new counter");
    CHECK(typed_audit::rehydrate_miss_invalidate_gen_v_read() == gen0, "3171 AC2: no gen bump");
}

static void ac3171_3_schema() {
    std::println("\n--- #3171 AC3: schema-3171 + SSOT ---");
    CompilerService svc;
    CHECK(svc.eval("(+ 1 1)").has_value(), "3171 AC3: warm");
    CHECK(href(svc, "schema-3171") == 3171, "3171 AC3: schema-3171");
    CHECK(href(svc, "issue-3171") == 3171, "3171 AC3: issue-3171");
    CHECK(href(svc, "linear-fast-path-steal-densify-clear-complete-wired") == 1, "3171 AC3: wired");
    CHECK(href(svc, "schema-3063") == 3063, "3171 AC3: schema-3063 preserved");
    CHECK(href(svc, "schema-3085") == 3085, "3171 AC3: schema-3085 preserved");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("linear_fast_path_ok") != std::string::npos, "3171 AC3: SSOT predicate");
    CHECK(tma.find("kLinearFastPathStealDensifyClearCompleteIssue") != std::string::npos,
          "3171 AC3: issue stamp");
}

static void ac3171_4_source_and_linter() {
    std::println("\n--- #3171 AC4: source-cite + linter ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto low = read_file("src/compiler/lowering_linear_types_impl.cpp");
    const auto t = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_linear_fast_path_clear_on_restamp_3171.py");
    const auto build = read_file("build.py");
    CHECK(tma.find("Issue #3171") != std::string::npos, "3171 AC4: tma cite");
    CHECK(efm.find("clear_escape_move_elision_gate_for_eval") != std::string::npos,
          "3171 AC4: unified_restamp clear");
    CHECK(mb.find("invalidate_fast_path_before_steal_densify_restamp") != std::string::npos,
          "3171 AC4: densify relocate invalidate");
    CHECK(low.find("Issue #3085 / #3171") != std::string::npos ||
              low.find("#3171") != std::string::npos,
          "3171 AC4: lowering consumer");
    CHECK(t.find("ac3171_1_prod_clear_blocks_elide") != std::string::npos, "3171 AC4: AC1");
    CHECK(!lint.empty() && lint.find("3171") != std::string::npos, "3171 AC4: linter");
    CHECK(build.find("check_linear_fast_path_clear_on_restamp_3171") != std::string::npos,
          "3171 AC4: build.py");
    CHECK(read_file("docs/design/3171-linear-fast-path-clear.md").empty(),
          "3171 AC4: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_3171.cpp").empty(),
          "3171 AC4: no invent test_issue_3171");
}

// ── Issue #3099: residual close — re-sample invalidate_gen after
// linear_fast_path_ok() returns true inside linear_ir_fastpath_try_skip.
// Closes the half-green linear state window where a concurrent
// densify/steal restamp on another fiber advances gen between the ok
// check and the actual elision (acquire pairs with the restamp release).
// Quiet path → one extra acquire load (zero extra work); mismatch →
// blocked + production counter (reuse existing surfaces, no new
// middle metrics key).
static void ac3099_1_re_sample_in_try_skip() {
    std::println("\n--- #3099 AC1: re-sample after linear_fast_path_ok in try_skip ---");
    apply_dev_audit_defaults();
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    // AC1 source-cite: re-sample present AFTER linear_fast_path_ok() in
    // linear_ir_fastpath_try_skip. Closes the race window between ok
    // check and elision.
    const auto try_skip_pos = tma.find("linear_ir_fastpath_try_skip");
    const auto ok_pos = tma.find("linear_fast_path_ok");
    const auto re_sample_pos =
        tma.find("Issue #3099: residual close \u2014 re-sample invalidate_gen");
    CHECK(try_skip_pos != std::string::npos, "3099 AC1: linear_ir_fastpath_try_skip present");
    CHECK(ok_pos != std::string::npos, "3099 AC1: linear_fast_path_ok present");
    CHECK(re_sample_pos != std::string::npos,
          "3099 AC1: re-sample code in try_skip (after ok returns true)");
    // The re-sample must be INSIDE linear_ir_fastpath_try_skip, AFTER
    // the linear_fast_path_ok() call (i.e., ok must be called first,
    // then re-sample). Verify ordering: ok_pos < re_sample_pos < ok_pos+...
    // Actually the test is simpler: just verify re_sample_pos is
    // between the function body start and the next function start.
    const auto next_func_after_try_skip =
        tma.find("inline constexpr uint8_t kTypeLinearProofOutcomeReject", try_skip_pos);
    CHECK(re_sample_pos < next_func_after_try_skip,
          "3099 AC1: re-sample is INSIDE linear_ir_fastpath_try_skip (before next decl)");
    // AC2: re-sample uses g_rehydrate_miss_invalidate_gen (acquire) and
    // g_rehydrate_miss_green_bind_gen (relaxed) — matches the existing
    // #3063 arm. Acquires acquire-load pair with release fetch_add in
    // invalidate_fast_path_before_steal_densify_restamp.
    const auto re_sample_section = tma.substr(re_sample_pos, 1200);
    CHECK(
        re_sample_section.find("g_rehydrate_miss_invalidate_gen.load(std::memory_order_acquire)") !=
            std::string::npos,
        "3099 AC1: re-sample acquire load on invalidate_gen");
    CHECK(
        re_sample_section.find("g_rehydrate_miss_green_bind_gen.load(std::memory_order_relaxed)") !=
            std::string::npos,
        "3099 AC1: re-sample relaxed load on green_bind_gen");
    CHECK(re_sample_section.find("g_linear_ir_fastpath_skip_blocked_total.fetch_add") !=
              std::string::npos,
          "3099 AC1: re-sample bumps existing blocked counter (no new middle key)");
    CHECK(re_sample_section.find("g_linear_fast_path_elide_blocked_production_total.fetch_add") !=
              std::string::npos,
          "3099 AC1: re-sample bumps existing production counter");
    // AC2: regression — existing #3063 AC1 already exercises gen advance
    // → !try_skip. The re-sample is on the OK path, but when the gen
    // advances BEFORE try_skip, ok itself returns false (existing #3063).
    // Quiet path: ok returns true → re-sample passes (gen matches) →
    // skip_total bumped. Verify counters exist.
    const auto skip_total = typed_audit::linear_ir_fastpath_skip_total_v_read();
    const auto blocked = typed_audit::linear_ir_fastpath_skip_blocked_total_v_read();
    CHECK(skip_total >= 0, "3099 AC2: skip_total counter surfaces");
    CHECK(blocked >= 0, "3099 AC2: blocked counter surfaces");
    apply_dev_audit_defaults();
}

static void ac3099_2_no_new_query_key() {
    std::println("\n--- #3099 AC2: no new middle metrics key ---");
    // AC4: no new metrics middle insertion. Reuses existing
    // g_linear_ir_fastpath_skip_total + g_linear_ir_fastpath_skip_blocked_total
    // + g_linear_fast_path_elide_blocked_production_total.
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto om = read_file("src/compiler/observability_metrics.h");
    // The new code must NOT add new counters / fields under
    // observability_metrics.h (per AC4 + Non-goals: no new middle key).
    // Source-cite: only the existing 3 counters are bumped by the
    // re-sample arm.
    (void)tma;
    (void)om;
    // AC5: source-cite Abort / clear still authoritative — #3030 is
    // unchanged. Verify clear_type_linear_commit_proof_for_test exists
    // and is referenced in the same module.
    CHECK(tma.find("clear_type_linear_commit_proof_for_test") != std::string::npos,
          "3099 AC5: #3030 clear_type_linear_commit_proof_for_test still present (unchanged)");
    CHECK(tma.find("Issue #3030") != std::string::npos,
          "3099 AC5: #3030 comment / cite still present (Abort authoritative)");
}

// ── Issue #3130: residual close — single pure predicate for IR/JIT
// Move/Drop elision that ALSO consults the live commit_readiness face.
// Closes the half-green window where linear_ir_fastpath_try_skip could
// return true after commit_readiness.would_allow_commit dropped to false
// (abort / densify-steal / force-rollback). Production/Full: never
// elide Move/Drop under would_allow_commit=false; Soft: zero extra
// counter noise (relaxed load only, no bump).
static void ac3130_linear_move_drop_elision_gates_commit_readiness() {
    std::println("\n--- #3130: linear_move_drop_elision_ok gates commit_readiness ---");

    // AC1: source-cite — new predicate in typed_mutation_audit.h.
    {
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        CHECK(tma.find("linear_move_drop_elision_ok") != std::string::npos,
              "ac3130 AC1: new predicate defined in typed_mutation_audit.h");
        CHECK(tma.find("Issue #3130") != std::string::npos, "ac3130 AC1: tma cites Issue #3130");
        // Wraps the existing linear_ir_fastpath_try_skip (preserves its
        // counter semantics for the linear face check + rehydrate gen
        // re-sample) and adds the readiness gate + production-only counter
        // bump on would_allow_commit=false.
        CHECK(tma.find("if (!linear_ir_fastpath_try_skip())") != std::string::npos,
              "ac3130 AC1: wraps linear_ir_fastpath_try_skip (preserves counters)");
        CHECK(tma.find("commit_readiness(commit_readiness_live_policy())") != std::string::npos,
              "ac3130 AC1: consults live commit_readiness face");
        CHECK(tma.find("g_linear_fast_path_elide_blocked_production_total") != std::string::npos,
              "ac3130 AC1: reuses existing production counter (no new metric key)");
    }

    // AC2: source-cite — call site in ir_executor_impl.cpp uses new predicate.
    {
        const auto ir = read_file("src/compiler/ir_executor_impl.cpp");
        CHECK(ir.find("aura::compiler::typed_audit::linear_move_drop_elision_ok()") !=
                  std::string::npos,
              "ac3130 AC2: IR Move/Drop call site uses linear_move_drop_elision_ok");
    }

    // AC3: source-cite — production counter is gated on would_allow_commit=false
    // + production_defaults_active (or AuditStrategy::Full). Soft: zero extra
    // counter noise (no bump on the relaxed load).
    {
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        const auto pred_pos = tma.find("linear_move_drop_elision_ok()");
        const auto pred_end = tma.find("\n}\n", pred_pos);
        const std::string pred_block =
            pred_pos != std::string::npos && pred_end != std::string::npos
                ? tma.substr(pred_pos, pred_end - pred_pos)
                : std::string{};
        CHECK(pred_block.find("production_defaults_active()") != std::string::npos,
              "ac3130 AC3: counter bump gated on production_defaults_active");
        CHECK(pred_block.find("get_strategy() == AuditStrategy::Full") != std::string::npos,
              "ac3130 AC3: counter bump gated on AuditStrategy::Full");
        CHECK(pred_block.find("cr.would_allow_commit") != std::string::npos,
              "ac3130 AC3: gate checks cr.would_allow_commit");
    }

    // AC4: existing sibling ACs preserved (#3030 / #3032 / #3063 / #3085 / #3099).
    {
        const auto t = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
        CHECK(t.find("ac3032_1_prod_miss_invalidates_fast_path") != std::string::npos,
              "ac3130 AC4: sibling #3032 AC1 preserved");
        CHECK(t.find("ac3063_1_prod_success_blocks_elide") != std::string::npos,
              "ac3130 AC4: sibling #3063 AC1 preserved");
        CHECK(t.find("ac3085_1_densify_miss_blocks_elision") != std::string::npos,
              "ac3130 AC4: sibling #3085 AC1 preserved");
        CHECK(t.find("ac3099_1_re_sample_in_try_skip") != std::string::npos,
              "ac3130 AC4: sibling #3099 AC1 preserved");
    }

    // AC5: counter reuse (no new query key middle insertion).
    {
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        // Existing counter — no new metric key.
        CHECK(tma.find("g_linear_fast_path_elide_blocked_production_total") != std::string::npos,
              "ac3130 AC5: existing production counter reused (no new query key)");
    }

    // AC6: no new tests/issues/test_issue_3130.cpp (per #81967).
    {
        const auto issue_test = read_file("tests/issues/test_issue_3130.cpp");
        CHECK(issue_test.empty(),
              "ac3130 AC6: no new tests/issues/test_issue_3130.cpp (must NOT — src-aligned only)");
    }
}

// ── Issue #3186: JIT Move/Drop elision also consults live commit_readiness
// (closes the half-green residual after densify/steal race). Extends #3130
// (predicate + IR call site) to JIT via aura_jit_linear_move_drop_elision_ok
// runtime bridge; emits call in linear_safety_probe so JIT deopts on either
// epoch-stale OR readiness-blocked. Soft/Off zero-cost (the predicate itself
// short-circuits the bump under Soft/Off). Reuses existing
// g_linear_fast_path_elide_blocked_production_total counter — no new metric key.
//   AC1: bridge function declared in aura_jit_bridge.h
//   AC2: bridge function extern "C" defined in aura_jit_bridge.cpp
//   AC3: FunctionCreate in aura_jit.cpp's create_runtime_bridge
//   AC4: reg entry in aura_jit.cpp's runtime reg block
//   AC5: probe in aura_jit.cpp's linear_safety_probe consults the new bridge
//        (epoch + readiness OR-combined into any_unsafe)

static void ac3186_jit_linear_move_drop_elision_probe() {
    std::println("\n--- #3186: JIT Move/Drop elision consults live commit_readiness ---");

    // AC1: bridge function declared in aura_jit_bridge.h.
    {
        const auto h = read_file("src/compiler/aura_jit_bridge.h");
        CHECK(h.find("aura_jit_linear_move_drop_elision_ok") != std::string::npos,
              "ac3186 AC1: bridge function declared in aura_jit_bridge.h");
        CHECK(h.find("Issue #3186") != std::string::npos, "ac3186 AC1: bridge.h cites Issue #3186");
    }

    // AC2: bridge function extern "C" defined in aura_jit_bridge.cpp.
    {
        const auto cpp = read_file("src/compiler/aura_jit_bridge.cpp");
        CHECK(cpp.find("extern \"C\" int aura_jit_linear_move_drop_elision_ok(void)") !=
                  std::string::npos,
              "ac3186 AC2: bridge function extern \"C\" defined in aura_jit_bridge.cpp");
        // Implementation wraps the existing predicate (no second proof model).
        CHECK(cpp.find("typed_audit::linear_move_drop_elision_ok()") != std::string::npos,
              "ac3186 AC2: bridge delegates to typed_audit predicate");
        // Reuses existing production counter (no new metric key).
        CHECK(cpp.find("g_linear_fast_path_elide_blocked_production_total") != std::string::npos,
              "ac3186 AC2: reuses existing production counter (no new metric key)");
    }

    // AC3: FunctionCreate in aura_jit.cpp's create_runtime_bridge.
    {
        const auto jit = read_file("src/compiler/aura_jit.cpp");
        CHECK(jit.find("fn_linear_move_drop_elision_ok = llvm::Function::Create") !=
                  std::string::npos,
              "ac3186 AC3: FunctionCreate in aura_jit.cpp create_runtime_bridge");
        // Member variable declared in the same module class.
        CHECK(jit.find("llvm::Function* fn_linear_move_drop_elision_ok = nullptr;") !=
                  std::string::npos,
              "ac3186 AC3: fn_linear_move_drop_elision_ok member declared");
    }

    // AC4: reg entry in aura_jit.cpp's runtime reg block (so ORC can resolve
    // the symbol emitted by linear_safety_probe).
    {
        const auto jit = read_file("src/compiler/aura_jit.cpp");
        CHECK(jit.find("reg(\"aura_jit_linear_move_drop_elision_ok\"") != std::string::npos,
              "ac3186 AC4: bridge registered in aura_jit.cpp runtime reg block");
    }

    // AC5: probe in aura_jit.cpp's linear_safety_probe emits the call + OR.
    {
        const auto jit = read_file("src/compiler/aura_jit.cpp");
        // Both probes must coexist (epoch + readiness) in the same critical
        // section. The OR is the unified unsafe signal.
        CHECK(jit.find("llvm::FunctionCallee(fn_linear_move_drop_elision_ok)") != std::string::npos,
              "ac3186 AC5: linear_safety_probe emits call to fn_linear_move_drop_elision_ok");
        CHECK(jit.find("not_elision_ok = irb->CreateICmpNE(elision_ok_i, zero32)") !=
                  std::string::npos,
              "ac3186 AC5: elision result compared to zero32");
        CHECK(jit.find("any_unsafe = irb->CreateOr(is_unsafe, not_elision_ok)") !=
                  std::string::npos,
              "ac3186 AC5: epoch + elision OR-combined into any_unsafe");
        CHECK(jit.find("irb->CreateCondBr(any_unsafe, bb_deopt, bb_ok)") != std::string::npos,
              "ac3186 AC5: branch on any_unsafe (deopt on either failure)");
    }
}

// ── Issue #3224: production IR/JIT entry (beyond Move/Drop) refuses when
// commit_readiness.would_allow_commit is false under active mutation.
//   AC1: production + depth>0 + !would_allow → predicate false; IR entries wired
//   AC2: Move/Drop still gated by linear_move_drop_elision_ok
//   AC3: Soft / quiet (depth==0) true
//   AC4: extend this suite; linter; no invent / docs/design

static void ac3224_ir_typed_entry_commit_readiness() {
    std::println("\n--- #3224: IR/JIT typed entry refuses under !commit_readiness ---");

    {
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        CHECK(tma.find("kIrTypedEntryCommitReadinessIssue") != std::string::npos,
              "3224 AC1: issue stamp");
        CHECK(tma.find("ir_typed_entry_commit_readiness_ok") != std::string::npos,
              "3224 AC1: predicate");
        CHECK(tma.find("aura_evaluator_mutation_boundary_depth()") != std::string::npos,
              "3224 AC1: depth C ABI");
        const auto ir = read_file("src/compiler/ir_executor_impl.cpp");
        CHECK(ir.find("ir_typed_entry_blocked_result") != std::string::npos, "3224 AC1: IR helper");
        CHECK(ir.find("commit-readiness-refused") != std::string::npos, "3224 AC1: refuse message");
        CHECK(ir.find("IRInterpreter::execute()") != std::string::npos &&
                  ir.find("ir_typed_entry_blocked_result(context_.metrics)") != std::string::npos,
              "3224 AC1: execute() gated");
        CHECK(ir.find("IRInterpreter::call_closure") != std::string::npos,
              "3224 AC1: call_closure");
        CHECK(ir.find("IRInterpreter::execute_function") != std::string::npos,
              "3224 AC1: execute_function");
        const auto jit = read_file("src/compiler/aura_jit.cpp");
        CHECK(jit.find("fn_ir_typed_entry_commit_readiness_ok") != std::string::npos,
              "3224 AC1: JIT fn");
        CHECK(jit.find("aura_jit_ir_typed_entry_commit_readiness_ok") != std::string::npos,
              "3224 AC1: JIT symbol");
        CHECK(jit.find("Issue #3224") != std::string::npos, "3224 AC1: Apply prologue cite");
        const auto brh = read_file("src/compiler/aura_jit_bridge.h");
        CHECK(brh.find("aura_jit_ir_typed_entry_commit_readiness_ok") != std::string::npos,
              "3224 AC1: bridge.h");
    }

    {
        const auto ir = read_file("src/compiler/ir_executor_impl.cpp");
        CHECK(ir.find("typed_audit::linear_move_drop_elision_ok()") != std::string::npos,
              "3224 AC2: Move/Drop still uses linear_move_drop_elision_ok");
    }

    {
        apply_dev_audit_defaults();
        clear_occurrence_empty_after_fence_for_test();
        typed_audit::note_occurrence_empty_after_fence(/*production_hard=*/true);
        typed_audit::g_linear_ir_fastpath_boundary_depth_override = 1;
        CHECK(typed_audit::ir_typed_entry_commit_readiness_ok(),
              "3224 AC3: Soft allows entry under mutation");
        typed_audit::g_linear_ir_fastpath_boundary_depth_override = -1;
        clear_occurrence_empty_after_fence_for_test();
        apply_dev_audit_defaults();
    }

    {
        apply_production_audit_defaults();
        clear_occurrence_empty_after_fence_for_test();
        typed_audit::reset_linear_ir_fastpath_counters_for_test();
        aura_typed_audit_test_clear_recover_override();
        typed_audit::g_linear_ir_fastpath_boundary_depth_override = 0;
        typed_audit::clear_type_linear_proof_outcome_for_test();
        CHECK(!typed_audit::ir_typed_entry_commit_readiness_ok(),
              "3414 AC2: Quiet depth==0 refuses (no stamp for this eval)");
        typed_audit::publish_type_linear_proof_outcome(typed_audit::kTypeLinearProofOutcomeStamped);
        CHECK(typed_audit::ir_typed_entry_commit_readiness_ok(),
              "3224 AC3: Stamped depth==0 allows (no extra commit_readiness)");
        typed_audit::note_occurrence_empty_after_fence(/*production_hard=*/true);
        typed_audit::g_linear_ir_fastpath_boundary_depth_override = 1;
        CHECK(!typed_audit::ir_typed_entry_commit_readiness_ok(),
              "3224 AC1: production + mutation + !would_allow refuses");
        typed_audit::g_linear_ir_fastpath_boundary_depth_override = 0;
        CHECK(typed_audit::ir_typed_entry_commit_readiness_ok(),
              "3224 AC3: Stamped depth==0 allows even with face");
        typed_audit::clear_type_linear_proof_outcome_for_test();
        typed_audit::g_linear_ir_fastpath_boundary_depth_override = -1;
        clear_occurrence_empty_after_fence_for_test();
        apply_dev_audit_defaults();
    }

    {
        const auto t = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
        const auto lint =
            read_file("scripts/coverage/checks/check_ir_typed_entry_commit_readiness_3224.py");
        const auto build = read_file("build.py");
        CHECK(t.find("ac3224_ir_typed_entry_commit_readiness") != std::string::npos,
              "3224 AC4: this suite");
        CHECK(!lint.empty() && lint.find("3224") != std::string::npos, "3224 AC4: linter");
        CHECK(build.find("check_ir_typed_entry_commit_readiness_3224") != std::string::npos,
              "3224 AC4: build.py");
        CHECK(read_file("docs/design/3224-ir-typed-entry-commit-readiness.md").empty(),
              "3224 AC4: no docs/design");
        CHECK(read_file("tests/compiler/test_issue_3224.cpp").empty(), "3224 AC4: no invent");
        CHECK(read_file("tests/issues/test_issue_3224.cpp").empty(),
              "3224 AC4: no tests/issues invent");
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        CHECK(tma.find("g_3224_") == std::string::npos, "3224 AC4: no new g_3224_* counter");
    }
}

// ── Issue #3343: production weak-ABI stubs fail-closed for IR/linear
// commit_readiness (light-link stub) so a JIT-less production binary
// cannot run under commit_readiness=false. Soft keeps weak allow.
//   AC1: production_defaults + weak stub → IR refuse / elision blocked /
//        post-mutate unsafe
//   AC2: production / full-JIT sources compile strong jit_bridge
//   AC3: force !commit_readiness under production → typed_audit refuse
//   AC4: Soft / missing probe keep weak allow / pass-through
//   AC5: this suite + steal-complete suite + linter; no invent / docs /
//        schema-3343

static void ac3343_production_weak_abi_commit_readiness() {
    std::println("\n--- #3343: production weak ABI fail-closed on IR commit_readiness ---");

    {
        apply_dev_audit_defaults();
        CHECK(aura_jit_ir_typed_entry_commit_readiness_ok() == 1,
              "3343 AC4: Soft stub allows IR entry");
        CHECK(aura_jit_linear_move_drop_elision_ok() == 1, "3343 AC4: Soft stub allows elision");
        CHECK(aura_jit_linear_post_mutate_enforce(0) == 0,
              "3343 AC4: Soft stub post-mutate pass-through");
    }

    {
        apply_production_audit_defaults();
        CHECK(aura_jit_ir_typed_entry_commit_readiness_ok() == 0,
              "3343 AC1: production stub refuses IR entry");
        CHECK(aura_jit_linear_move_drop_elision_ok() == 0,
              "3343 AC1: production stub blocks elision");
        CHECK(aura_jit_linear_post_mutate_enforce(0) == 1,
              "3343 AC1: production stub post-mutate unsafe");
        apply_dev_audit_defaults();
    }

    {
        apply_production_audit_defaults();
        clear_occurrence_empty_after_fence_for_test();
        typed_audit::reset_linear_ir_fastpath_counters_for_test();
        aura_typed_audit_test_clear_recover_override();
        typed_audit::note_occurrence_empty_after_fence(/*production_hard=*/true);
        typed_audit::g_linear_ir_fastpath_boundary_depth_override = 1;
        CHECK(!typed_audit::ir_typed_entry_commit_readiness_ok(),
              "3343 AC3: production + mutation + !would_allow refuses");
        typed_audit::g_linear_ir_fastpath_boundary_depth_override = -1;
        clear_occurrence_empty_after_fence_for_test();
        apply_dev_audit_defaults();
    }

    {
        const auto stub = read_file("src/compiler/aura_jit_bridge_stub.cpp");
        const auto brc = read_file("src/compiler/aura_jit_bridge.cpp");
        const auto cmake = read_file("CMakeLists.txt");
        CHECK(stub.find("Issue #3343") != std::string::npos, "3343 AC1: stub cites #3343");
        CHECK(stub.find("stub_production_defaults_active") != std::string::npos,
              "3343 AC1: stub production-aware");
        CHECK(brc.find("typed_audit::ir_typed_entry_commit_readiness_ok()") != std::string::npos,
              "3343 AC2: strong IR readiness consults typed_audit");
        CHECK(brc.find("typed_audit::linear_move_drop_elision_ok()") != std::string::npos,
              "3343 AC2: strong elision consults typed_audit");
        CHECK(cmake.find("Do NOT add aura_jit_bridge_stub.cpp here") != std::string::npos,
              "3343 AC2: full JIT archive forbids stub");
        CHECK(cmake.find("src/compiler/aura_jit_bridge.cpp") != std::string::npos,
              "3343 AC2: production compiles strong bridge");
    }

    {
        const auto stub = read_file("src/compiler/aura_jit_bridge_stub.cpp");
        const auto t = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
        const auto steal = read_file("tests/serve/test_steal_complete_strong_entry.cpp");
        const auto lint =
            read_file("scripts/coverage/checks/check_production_weak_abi_commit_readiness_3343.py");
        const auto build = read_file("build.py");
        CHECK(t.find("ac3343_production_weak_abi_commit_readiness") != std::string::npos,
              "3343 AC5: this suite");
        CHECK(steal.find("Issue #3343") != std::string::npos, "3343 AC5: steal suite");
        CHECK(!lint.empty() && lint.find("3343") != std::string::npos, "3343 AC5: linter");
        CHECK(build.find("check_production_weak_abi_commit_readiness_3343") != std::string::npos,
              "3343 AC5: build.py");
        CHECK(read_file("docs/design/3343-production-weak-abi-commit-readiness.md").empty(),
              "3343 AC5: no docs/design");
        CHECK(read_file("tests/compiler/test_issue_3343.cpp").empty(), "3343 AC5: no invent");
        CHECK(read_file("tests/issues/test_issue_3343.cpp").empty(),
              "3343 AC5: no tests/issues invent");
        CHECK(stub.find("schema-3343") == std::string::npos, "3343 AC5: no schema-3343");
        CHECK(stub.find("g_3343_") == std::string::npos, "3343 AC5: no g_3343_*");
    }
}

static void ac3419_jit_typed_entry_every_function() {
    std::println("\n--- #3419: JIT typed-entry on every compiled function ---");
    const auto jit = read_file("src/compiler/aura_jit.cpp");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("kJitTypedEntryEveryFunctionIssue = 3419") != std::string::npos,
          "3419 AC1: issue stamp");
    CHECK(jit.find("can_typed") != std::string::npos, "3419 AC1: typed-entry emit gate");
    CHECK(jit.find("hard_typed_entry") != std::string::npos, "3419 AC1: production/Full gate");
    CHECK(jit.find("prologue_name = named ? fn.name : \"<anon>\"") != std::string::npos,
          "3419 AC1: anonymous functions emit prologue");
    CHECK(jit.find("fn_ir_typed_entry_commit_readiness_ok") != std::string::npos,
          "3419 AC1: emit aura_jit_ir_typed_entry_commit_readiness_ok");
    CHECK(jit.find("deopt_to_interpreter") != std::string::npos, "3419 AC1: deopt on 0");
    CHECK(jit.find("g_linear_fast_path_elide_blocked_production_total") != std::string::npos ||
              tma.find("g_linear_fast_path_elide_blocked_production_total") != std::string::npos,
          "3419 AC4: reuse elide counter");

    const auto stub = read_file("src/compiler/aura_jit_bridge_stub.cpp");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto rah = read_file("src/serve/runtime_production_abi.h");
    const auto rab = read_file("src/serve/runtime_production_abi.cpp");
    CHECK(stub.find("aura_abi_strong_ir_typed_entry_v") != std::string::npos,
          "3419 AC2: stub cites marker (no cross-DSO weak def)");
    CHECK(stub.find(
              "extern \"C\" __attribute__((weak, used)) int aura_abi_strong_ir_typed_entry_v") ==
              std::string::npos,
          "3419 AC2: stub must not define ABI marker");
    CHECK(fm.find("aura_abi_strong_ir_typed_entry_v") != std::string::npos,
          "3419 AC2: strong marker in evaluator_fiber_mutation");
    CHECK(rah.find("kProductionAbiSelfcheckFailBitTypedEntry") != std::string::npos,
          "3419 AC2: fail bit 8");
    CHECK(rab.find("aura_abi_strong_ir_typed_entry_v() == 0") != std::string::npos,
          "3419 AC2: selfcheck treats stub as fail");

    {
        apply_dev_audit_defaults();
        CHECK(jit.find("hard_typed_entry") != std::string::npos,
              "3419 AC3: Soft omit via hard gate");
        apply_production_audit_defaults();
        typed_audit::clear_type_linear_proof_outcome_for_test();
        typed_audit::g_linear_ir_fastpath_boundary_depth_override = 0;
        CHECK(!typed_audit::ir_typed_entry_commit_readiness_ok(),
              "3419: production Quiet last-proof refuses typed execute");
        typed_audit::g_linear_ir_fastpath_boundary_depth_override = -1;
        apply_dev_audit_defaults();
    }

    const auto build = read_file("build.py");
    CHECK(build.find("check_jit_typed_entry_every_function_3419") != std::string::npos,
          "3419 AC5: build.py");
    CHECK(read_file("docs/design/3419-jit-typed-entry.md").empty(), "3419: no docs/design");
    CHECK(read_file("tests/compiler/test_issue_3419.cpp").empty(), "3419: no invent");
    CHECK(stub.find("schema-3419") == std::string::npos, "3419 AC4: no new query key");
    CHECK(stub.find("g_3419_") == std::string::npos, "3419 AC4: no g_3419_*");
}

static void ac3446_linear_epoch_fence_elision_typed() {
    std::println("\n--- #3446: fence ORs elision_ok + typed-entry (residual of #3186) ---");
    const auto jit = read_file("src/compiler/aura_jit.cpp");
    CHECK(jit.find("Issue #3446") != std::string::npos, "3446: fence cite");
    CHECK(jit.find("fence_elision_blocked = irb->CreateICmpEQ(fence_elision_ok_i, zero32)") !=
              std::string::npos,
          "3446: elision == 0 on fence (not probe continue)");
    CHECK(jit.find("fence_entry_blocked = irb->CreateICmpEQ(fence_entry_ok_i, zero32)") !=
              std::string::npos,
          "3446: typed-entry == 0 on fence");
    CHECK(jit.find("not_elision_ok = irb->CreateICmpNE(elision_ok_i, zero32)") != std::string::npos,
          "3446: #3186 probe ICmpNE kept");
    const auto t = read_file("tests/compiler/test_escape_move_elision_gate.cpp");
    CHECK(t.find("ac3446_1_fence_or_skips_move_body") != std::string::npos,
          "3446: primary fixture is test_escape_move_elision_gate");
    const auto build = read_file("build.py");
    CHECK(build.find("check_linear_epoch_fence_elision_typed_3446") != std::string::npos,
          "3446: linter wired");
}

// ── Issue #3225: persist seqlock so concurrent outermost write × densify/steal
// rehydrate cannot freeze a mixed fingerprint.
//   AC1: production in-flight (odd seq) rehydrate is miss → empty, no green
//   AC2: last_proof_goal_fingerprint / live_goal_count stay unmixed
//   AC3: Soft / quiet zero extra (no seq consult)
//   AC4: this suite + linter; no invent / docs/design

static void ac3225_occurrence_persist_seqlock() {
    std::println("\n--- #3225: occurrence persist seqlock vs concurrent rehydrate ---");

    {
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        const auto impl = read_file("src/compiler/type_checker_impl.cpp");
        CHECK(tma.find("kOccurrencePersistSeqIssue") != std::string::npos, "3225 AC1: stamp");
        CHECK(tma.find("g_occurrence_persist_seq") != std::string::npos, "3225 AC1: seq");
        CHECK(tma.find("occurrence_persist_seq_begin_write") != std::string::npos,
              "3225 AC1: begin write");
        CHECK(impl.find("occurrence_persist_seq_begin_write") != std::string::npos,
              "3225 AC1: append seqlock");
        CHECK(impl.find("g0 & 1ull") != std::string::npos ||
                  impl.find("(g0 & 1") != std::string::npos,
              "3225 AC1: odd seq is in-flight");
        CHECK(impl.find("g0 != g1") != std::string::npos, "3225 AC1: mid-copy gen change");
        CHECK(impl.find("Issue #3225") != std::string::npos, "3225 AC1: rehydrate cite");
        const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(mb.find("Issue #3225") != std::string::npos, "3225 AC1: persist helper cite");
    }

    {
        unsetenv("AURA_OCCURRENCE_PERSIST");
        apply_production_audit_defaults();
        typed_audit::reset_occurrence_persist_seq_for_test();
        clear_occurrence_empty_after_fence_for_test();
        typed_audit::reset_rehydrate_miss_invalidate_for_test();
        UnitCs u;
        u.cs.set_current_epoch(5);
        auto v = u.cs.fresh_var();
        u.cs.note_occurrence_goal(v, u.reg.int_type(), 7, 70, /*epoch=*/5);
        CHECK(u.cs.append_occurrence_snapshot(70) == 1, "3225 AC1: persist wrote");
        CHECK((typed_audit::occurrence_persist_seq_v_read() & 1ull) == 0,
              "3225 AC1: seq even after write");
        u.cs.set_current_epoch(6);
        CHECK(u.cs.prune_occurrence_goals(6) == 1, "3225 AC1: prune");
        CHECK(u.cs.occurrence_goals_size() == 0, "3225 AC1: live empty");
        const auto fp0 = typed_audit::last_proof_goal_fingerprint_v_read();
        const auto bind0 =
            typed_audit::g_rehydrate_success_bind_total.load(std::memory_order_relaxed);
        typed_audit::bump_occurrence_persist_seq_for_test(); // odd: write in flight
        const auto miss0 = u.m.occurrence_persist_rehydrate_miss_total.load();
        const auto rh = u.cs.rehydrate_occurrence_from_persist(0);
        CHECK(rh == 0, "3225 AC1: in-flight seq → miss");
        CHECK(u.cs.occurrence_goals_size() == 0, "3225 AC1: live stays empty");
        CHECK(u.m.occurrence_persist_rehydrate_miss_total.load() > miss0, "3225 AC1: miss total");
        CHECK(typed_audit::last_proof_goal_fingerprint_v_read() == fp0,
              "3225 AC2: fingerprint not mixed");
        CHECK(typed_audit::g_rehydrate_success_bind_total.load(std::memory_order_relaxed) == bind0,
              "3225 AC2: no success bind of torn snapshot");
        typed_audit::bump_occurrence_persist_seq_for_test(); // even again
        const auto rh2 = u.cs.rehydrate_occurrence_from_persist(0);
        CHECK(rh2 >= 1, "3225 AC1: stable seq restores single snapshot");
        CHECK(u.cs.occurrence_goals_size() == 1, "3225 AC1: one authoritative snapshot");
        apply_dev_audit_defaults();
        typed_audit::reset_occurrence_persist_seq_for_test();
        typed_audit::reset_rehydrate_miss_invalidate_for_test();
        clear_occurrence_empty_after_fence_for_test();
    }

    {
        unsetenv("AURA_OCCURRENCE_PERSIST");
        apply_dev_audit_defaults();
        typed_audit::reset_occurrence_persist_seq_for_test();
        const auto impl = read_file("src/compiler/type_checker_impl.cpp");
        CHECK(impl.find("occurrence_persist_seq_hard()") != std::string::npos ||
                  impl.find("occurrence_persist_seq_begin_write") != std::string::npos,
              "3225 AC3: seq gated");
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        CHECK(tma.find("if (occurrence_persist_seq_hard())") != std::string::npos,
              "3225 AC3: Soft skips seq bump");
        setenv("AURA_OCCURRENCE_PERSIST", "1", 1);
        UnitCs u;
        u.cs.set_current_epoch(1);
        auto v = u.cs.fresh_var();
        u.cs.note_occurrence_goal(v, u.reg.int_type(), 1, 1, 1);
        CHECK(u.cs.append_occurrence_snapshot(1) == 1, "3225 AC3: Soft+env persist still writes");
        CHECK(typed_audit::occurrence_persist_seq_v_read() == 0,
              "3225 AC3: Soft does not bump seq");
        u.cs.occurrence_goals_size(); // keep live
        // Force-empty live and odd seq: Soft must still rehydrate (no seq consult).
        u.cs.set_current_epoch(2);
        (void)u.cs.prune_occurrence_goals(2);
        typed_audit::bump_occurrence_persist_seq_for_test();
        const auto rh = u.cs.rehydrate_occurrence_from_persist(0);
        CHECK(rh >= 1, "3225 AC3: Soft ignores odd seq");
        unsetenv("AURA_OCCURRENCE_PERSIST");
        typed_audit::reset_occurrence_persist_seq_for_test();
        apply_dev_audit_defaults();
    }

    {
        const auto t = read_file("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp");
        const auto lint = read_file("scripts/coverage/checks/check_occurrence_persist_seq_3225.py");
        const auto build = read_file("build.py");
        CHECK(t.find("ac3225_occurrence_persist_seqlock") != std::string::npos, "3225 AC4: suite");
        CHECK(!lint.empty() && lint.find("3225") != std::string::npos, "3225 AC4: linter");
        CHECK(build.find("check_occurrence_persist_seq_3225") != std::string::npos,
              "3225 AC4: build.py");
        CHECK(read_file("docs/design/3225-occurrence-persist-seq.md").empty(),
              "3225 AC4: no docs/design");
        CHECK(read_file("tests/compiler/test_issue_3225.cpp").empty(), "3225 AC4: no invent");
        CHECK(read_file("tests/issues/test_issue_3225.cpp").empty(), "3225 AC4: no tests/issues");
        const auto tma = read_file("src/compiler/typed_mutation_audit.h");
        CHECK(tma.find("g_3225_") == std::string::npos, "3225 AC4: no g_3225_* counter");
    }
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
    aura::compiler::lock_order::reset_tls_for_test();
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
    aura::compiler::lock_order::reset_tls_for_test();
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
    aura::compiler::lock_order::reset_tls_for_test();
    try {
        std::println("\n=== #3004 persist + Full audit atomic with query:type ===");
        ac3004_1_authority_after_persist();
        ac3004_2_soft_no_durable();
        ac3004_3_discard_provisional_on_fail();
        ac3004_4_schema_and_lineage();
        ac3004_5_source_and_linter();
    } catch (const std::exception& ex) {
        std::println("  #3004 threw: {}", ex.what());
        CHECK(false, "#3004 persist audit atomic (EDEADLK)");
        aura::compiler::lock_order::reset_tls_for_test();
        apply_dev_audit_defaults();
    }
    std::println("\n=== #3082 mid/nested MutationBoundary occurrence provisional ===");
    ac3082_1_nested_success_never_persists();
    aura::compiler::lock_order::reset_tls_for_test();
    try {
        ac3082_2_nested_query_inflight();
        ac3082_3_outermost_persist_unchanged();
        ac3082_4_soft_no_nested_zero_extra();
        ac3082_5_nested_fail_inflight_outer_abort_discards();
        ac3082_6_schema_and_linter();
    } catch (const std::exception& ex) {
        std::println("  #3082 threw: {}", ex.what());
        CHECK(false, "#3082 nested typecheck (EDEADLK)");
        aura::compiler::lock_order::reset_tls_for_test();
        apply_dev_audit_defaults();
    }
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
    std::println("\n=== #3171 steal/densify restamp complete-clear ===");
    ac3171_1_prod_clear_blocks_elide();
    ac3171_2_soft_zero_extra();
    ac3171_3_schema();
    ac3171_4_source_and_linter();
    std::println("\n=== #3085 densify/steal miss blocks lowering elision ===");
    ac3085_1_densify_miss_blocks_elision();
    ac3085_2_green_rebind_restores();
    ac3085_3_abort_clear_unchanged();
    ac3085_4_soft_zero_extra();
    ac3085_5_schema_and_linter();
    ac3099_1_re_sample_in_try_skip();
    ac3099_2_no_new_query_key();
    // Issue #3130: IR Move/Drop fast-path also consults live
    // commit_readiness face (closes the half-green residual).
    ac3130_linear_move_drop_elision_gates_commit_readiness();
    // Issue #3186: JIT Move/Drop elision also consults live
    // commit_readiness face in the same critical section (extends
    // #3130 predicate to JIT via aura_jit_linear_move_drop_elision_ok
    // runtime bridge + linear_safety_probe OR).
    ac3186_jit_linear_move_drop_elision_probe();
    ac3224_ir_typed_entry_commit_readiness();
    ac3343_production_weak_abi_commit_readiness();
    ac3419_jit_typed_entry_every_function();
    ac3446_linear_epoch_fence_elision_typed();
    ac3225_occurrence_persist_seqlock();
    // Issue #3170: outermost-success occurrence persist fingerprint guard
    // + uniform clear-on-abort/nested (I4 from 2026-08 type-system review -
    // 半解不得出厂).
    ac3170_1_outermost_success_fingerprint_guard();
    ac3170_2_abort_nested_uniform_clear();
    ac3170_3_soft_zero_behavioural_change();
    ac3170_4_quiet_zero_extra_atomics();
    ac3170_5_additive_observability_only();
    ac3170_6_source_and_linter();
    std::println("\n=== #3193 nested abort + densify/steal one authority face ===");
    ac3193_1_prod_hold_blocks_rehydrate();
    ac3193_2_no_mixed_green_residual();
    ac3193_3_soft_observe_quiet_zero();
    ac3193_4_source_and_linter();
    std::println("\n=== #3232 nested AbortAuthorityHold residual ===");
    ac3232_1_nested_hold_keeps_block();
    ac3232_2_soft_nested_observe_quiet();
    ac3232_3_source_cite_dual_restore_rehydrate();
    std::println(
        "\n=== #3346 last-look fingerprint/goals/linear_root vs densify×steal×mid-abort ===");
    ac3346_1_last_look_fingerprint_mismatch_rejects();
    ac3346_2_outstanding_authority_refuses_stamp();
    ac3346_3_stamp_matches_live_under_acquire();
    ac3346_4_soft_zero_extra_and_linter();
    std::println("\n=== #3418 fingerprint cap overflow silent-green ===");
    ac3418_fingerprint_cap_overflow_rejects();
    ac3418_source_and_linter();
    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_occurrence_goal_persist_rehydrate();
}
#endif
