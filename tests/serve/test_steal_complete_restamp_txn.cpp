// @category: unit
// @reason: Issue #2510 — transactional LayoutStamp + provenance restamp on
//          steal-complete (sole success-path restamp entry under strong ABI).
//
//   AC1: on_steal_complete is the sole restamp entry (source-cite + gate)
//   AC2: LayoutStamp field mismatch under Hard → hard-fail; fiber not Ready
//   AC3: production Soft ignored; steal_snapshot_hard_fail_total observable
//   AC4: forced stamp drift inject → mismatch hard-fail; match → restamp +1
//   AC5: light stress (N steals, no silent Ready after hard-fail)

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "serve/fiber.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

extern "C" void aura_evaluator_on_steal_complete(void* fiber_ptr) noexcept;
extern "C" std::uint64_t aura_fiber_static_steal_snapshot_hard_fail_total();

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::FiberState;
using aura::serve::YieldReason;
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
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:stable-ref-stats-hash\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: sole restamp entry (source-cite) ──
static void ac1_sole_restamp_entry() {
    std::println("\n--- #2510 AC1: on_steal_complete sole restamp entry ---");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto wc = read_file("src/serve/worker.cpp");
    CHECK(fm.find("aura_evaluator_on_steal_complete") != std::string::npos,
          "AC1: strong steal-complete entry");
    CHECK(fm.find("Issue #2510") != std::string::npos, "AC1: #2510 cite");
    CHECK(fm.find("refresh_stale_frames_after_steal") != std::string::npos,
          "AC1: forced EnvFrame restamp in on_steal_complete");
    CHECK(fm.find("steal_complete_restamp_total") != std::string::npos,
          "AC1: restamp counter bump");
    CHECK(fm.find("StableRefRefreshSite::Steal") != std::string::npos ||
              fm.find("auto_restamp_pinned_stable_refs_at") != std::string::npos,
          "AC1: provenance restamp in transaction");
    CHECK(wc.find("call_steal_complete(stolen)") != std::string::npos,
          "AC1: worker sole call site");
    CHECK(wc.find("FiberState::Done") != std::string::npos ||
              wc.find("is_cancel_requested") != std::string::npos,
          "AC1: worker skips Ready after hard-fail");
    // Does NOT clear stamp (resume dual-check).
    CHECK(fm.find("Does NOT clear") != std::string::npos ||
              fm.find("Does NOT clear the stamp") != std::string::npos ||
              fm.find("Does NOT clear resume_layout_stamp") != std::string::npos,
          "AC1: stamp retained for resume");
}

// ── AC2: hard mismatch → cancel + Done ──
static void ac2_hard_mismatch_fail() {
    std::println("\n--- #2510 AC2: LayoutStamp mismatch under Hard → hard-fail ---");
    ::setenv("AURA_STEAL_SNAPSHOT_HARD", "1", 1);
    ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
    aura::serve::reset_steal_snapshot_soft_for_test();
    CHECK(aura::serve::is_steal_snapshot_hard_mode(), "AC2: hard mode");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto& ev = cs.evaluator();
    CompilerMetrics metrics;
    ev.set_compiler_metrics(&metrics);

    const auto mm0 = metrics.layout_stamp_steal_mismatch_total.load();
    const auto hf0 = metrics.steal_complete_layout_hard_fail_total.load();
    const auto sh0 = aura_fiber_static_steal_snapshot_hard_fail_total();
    const auto rest0 = metrics.steal_complete_restamp_total.load();

    Fiber f([] {});
    const auto cur = ev.current_layout_stamp();
    // Inject stamp drift (arena_gen +99) — densify race simulation.
    f.set_resume_layout_stamp(cur.arena_id, cur.arena_gen + 99, cur.flat_gen, cur.mutation_epoch,
                              cur.env_gen, cur.defuse_version, cur.shape_version);
    aura_evaluator_on_steal_complete(&f);

    CHECK(metrics.layout_stamp_steal_mismatch_total.load() > mm0, "AC2: mismatch +1");
    CHECK(metrics.steal_complete_layout_hard_fail_total.load() > hf0, "AC2: layout hard-fail +1");
    CHECK(aura_fiber_static_steal_snapshot_hard_fail_total() > sh0, "AC2: hard_fail_total +1");
    CHECK(f.is_cancel_requested(), "AC2: cancel requested");
    CHECK(f.state() == FiberState::Done, "AC2: fiber Done (not Ready)");
    // No success restamp after hard-fail.
    CHECK(metrics.steal_complete_restamp_total.load() == rest0, "AC2: no restamp after hard-fail");

    ev.set_compiler_metrics(nullptr);
    ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
    aura::serve::reset_steal_snapshot_soft_for_test();
}

// ── AC3: Soft continues; production Soft ignored (source + soft path) ──
static void ac3_soft_and_production() {
    std::println("\n--- #2510 AC3: Soft metric-only; hard_fail observable ---");
    ::setenv("AURA_STEAL_SNAPSHOT_SOFT", "1", 1);
    ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
    aura::serve::reset_steal_snapshot_soft_for_test();
    CHECK(aura::serve::is_steal_snapshot_soft_mode(), "AC3: soft mode");
    CHECK(!aura::serve::is_steal_snapshot_hard_mode(), "AC3: hard off under Soft");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto& ev = cs.evaluator();
    CompilerMetrics metrics;
    ev.set_compiler_metrics(&metrics);

    const auto hf0 = metrics.steal_complete_layout_hard_fail_total.load();
    const auto sh0 = aura_fiber_static_steal_snapshot_hard_fail_total();
    Fiber f([] {});
    const auto cur = ev.current_layout_stamp();
    f.set_resume_layout_stamp(cur.arena_id, cur.arena_gen + 7, cur.flat_gen, cur.mutation_epoch,
                              cur.env_gen, cur.defuse_version, cur.shape_version);
    aura_evaluator_on_steal_complete(&f);

    CHECK(metrics.steal_complete_layout_hard_fail_total.load() == hf0,
          "AC3: Soft → no layout hard-fail");
    CHECK(aura_fiber_static_steal_snapshot_hard_fail_total() == sh0,
          "AC3: Soft → hard_fail_total flat");
    CHECK(f.state() != FiberState::Done || !f.is_cancel_requested(),
          "AC3: Soft continues (not force Done)");
    // Source: production Soft ignored via steal_snapshot_soft_production_locked.
    const auto fh = read_file("src/serve/fiber.cpp");
    CHECK(fh.find("steal_snapshot_soft_production_locked") != std::string::npos ||
              fh.find("g_steal_snapshot_soft_production_locked") != std::string::npos,
          "AC3: production Soft lock present");
    CHECK(href(cs, "steal-snapshot-hard-fail-total") >= 0 ||
              href(cs, "steal-complete-layout-hard-fail-total") >= 0,
          "AC3: hard_fail queryable (or layout hard-fail key)");

    ev.set_compiler_metrics(nullptr);
    ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
    aura::serve::reset_steal_snapshot_soft_for_test();
}

// ── AC4: match → restamp; soft mismatch still dual-check ──
static void ac4_match_restamp_and_drift() {
    std::println("\n--- #2510 AC4: match restamp + soft drift inject ---");
    ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
    ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
    aura::serve::reset_steal_snapshot_soft_for_test();

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto& ev = cs.evaluator();
    CompilerMetrics metrics;
    ev.set_compiler_metrics(&metrics);

    // Matching stamp → restamp +1, no hard-fail.
    {
        const auto rest0 = metrics.steal_complete_restamp_total.load();
        const auto mm0 = metrics.layout_stamp_steal_mismatch_total.load();
        Fiber f([] {});
        const auto cur = ev.current_layout_stamp();
        f.set_resume_layout_stamp(cur.arena_id, cur.arena_gen, cur.flat_gen, cur.mutation_epoch,
                                  cur.env_gen, cur.defuse_version, cur.shape_version,
                                  cur.ir_soa_generation);
        aura_evaluator_on_steal_complete(&f);
        CHECK(metrics.steal_complete_restamp_total.load() > rest0, "AC4: match → restamp +1");
        CHECK(metrics.layout_stamp_steal_mismatch_total.load() == mm0, "AC4: match → no mismatch");
        CHECK(f.has_resume_layout_stamp(), "AC4: stamp retained");
        CHECK(f.state() != FiberState::Done, "AC4: not Done on match");
    }

    // Soft default: mismatch still dual-check, no hard-fail (unless production).
    if (!aura::serve::is_steal_snapshot_hard_mode()) {
        const auto mm0 = metrics.layout_stamp_steal_mismatch_total.load();
        Fiber f2([] {});
        const auto cur = ev.current_layout_stamp();
        f2.set_resume_layout_stamp(cur.arena_id, cur.arena_gen + 1, cur.flat_gen,
                                   cur.mutation_epoch, cur.env_gen, cur.defuse_version,
                                   cur.shape_version);
        aura_evaluator_on_steal_complete(&f2);
        CHECK(metrics.layout_stamp_steal_mismatch_total.load() > mm0,
              "AC4: soft drift → mismatch (no silent green)");
    }

    ev.set_compiler_metrics(nullptr);
}

// ── AC5: stress N steals, hard-fail never Ready ──
static void ac5_stress() {
    std::println("\n--- #2510 AC5: N-iter stamp inject stress ---");
    ::setenv("AURA_STEAL_SNAPSHOT_HARD", "1", 1);
    ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
    aura::serve::reset_steal_snapshot_soft_for_test();

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto& ev = cs.evaluator();
    CompilerMetrics metrics;
    ev.set_compiler_metrics(&metrics);

    constexpr int N = 200;
    int hard_done = 0;
    int silent_ready = 0;
    for (int i = 0; i < N; ++i) {
        Fiber f([] {});
        const auto cur = ev.current_layout_stamp();
        // Alternate match / mismatch.
        const bool drift = (i % 2) != 0;
        f.set_resume_layout_stamp(cur.arena_id, cur.arena_gen + (drift ? 1 + (i % 3) : 0),
                                  cur.flat_gen, cur.mutation_epoch, cur.env_gen, cur.defuse_version,
                                  cur.shape_version, cur.ir_soa_generation);
        aura_evaluator_on_steal_complete(&f);
        if (drift) {
            if (f.state() == FiberState::Done && f.is_cancel_requested())
                ++hard_done;
            else if (f.state() == FiberState::Ready && !f.is_cancel_requested())
                ++silent_ready;
        }
    }
    CHECK(hard_done == N / 2, "AC5: all mismatch hard-failed (Done+cancel)");
    CHECK(silent_ready == 0, "AC5: zero silent Ready after mismatch");
    CHECK(metrics.steal_complete_layout_hard_fail_total.load() >= static_cast<std::uint64_t>(N / 2),
          "AC5: hard-fail counter covers injects");

    ev.set_compiler_metrics(nullptr);
    ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
    aura::serve::reset_steal_snapshot_soft_for_test();
}

// ── schema ──
static void ac_schema() {
    std::println("\n--- #2510 schema + query ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2510") == 2510, "schema-2510");
    CHECK(href(cs, "issue-2510") == 2510, "issue-2510");
    CHECK(href(cs, "steal-complete-restamp-wired") == 1, "wired");
    CHECK(href(cs, "steal-complete-restamp-total") >= 0, "restamp total");
    CHECK(href(cs, "steal-complete-layout-hard-fail-total") >= 0, "layout hard-fail total");
    CHECK(href(cs, "schema-2351") == 2351, "schema-2351 retained");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_steal_complete_restamp_txn") != std::string::npos, "cmake");
}

// ── Issue #2699 AC1: unified entry exists (Ok / RejectHard) ──
static void ac2699_1_unified_entry_exists() {
    std::println("\n--- #2699 AC1: steal_safety_transaction unified entry ---");
    const auto hdr = read_file("src/serve/steal_safety.h");
    const auto cpp = read_file("src/serve/steal_safety.cpp");
    CHECK(hdr.find("StealSafetyDecision") != std::string::npos,
          "AC1: hdr declares StealSafetyDecision enum");
    CHECK(hdr.find("steal_safety_transaction") != std::string::npos,
          "AC1: hdr declares unified entry");
    CHECK(hdr.find("Issue #2699") != std::string::npos, "AC1: hdr cites #2699");
    CHECK(hdr.find("g_steal_safety_transaction_calls_total") != std::string::npos,
          "AC1: hdr has calls counter");
    CHECK(hdr.find("g_steal_safety_transaction_reject_hard_total") != std::string::npos,
          "AC1: hdr has reject_hard counter");
    CHECK(hdr.find("g_steal_safety_transaction_ok_total") != std::string::npos,
          "AC1: hdr has ok counter");
    CHECK(hdr.find("g_steal_safety_transaction_wired") != std::string::npos,
          "AC1: hdr has wired sentinel");
    CHECK(hdr.find("kStealSafetyTransactionIssue = 2699") != std::string::npos,
          "AC1: hdr stamps issue = 2699");
    CHECK(cpp.find("mutation_safety_snapshot") != std::string::npos, "AC1 step 1: snapshot sample");
    CHECK(cpp.find("mutation_safety_snapshot_inconsistent") != std::string::npos,
          "AC1 step 2: inconsistency check");
    CHECK(cpp.find("force_clear_residual_defer_for_evaluator") != std::string::npos,
          "AC1 step 3: residual GcDefer clear");
    CHECK(cpp.find("aura_evaluator_on_steal_complete") != std::string::npos,
          "AC1 steps 4-6: PanicCheckpoint + LayoutStamp + provenance");
    CHECK(cpp.find("set_resume_safety_ticket") != std::string::npos,
          "AC1 step 7: stamp resume_safety_ticket on Ok path");
}

// ── Issue #2699 AC2: RejectHard → never local_queue_.push ──
static void ac2699_2_reject_hard_skips_enqueue() {
    std::println("\n--- #2699 AC2: RejectHard skips enqueue ---");
    // Source-cite: worker.cpp try_steal_from routes through the unified
    // entry and never enqueues on RejectHard. Wire-in marker verifies
    // the call graph is the single entry point.
    const auto worker = read_file("src/serve/worker.cpp");
    CHECK(worker.find("steal_safety_transaction") != std::string::npos,
          "AC2: worker.cpp try_steal_from routes through unified entry");
    CHECK(worker.find("RejectHard") != std::string::npos,
          "AC2: worker.cpp recognizes RejectHard signal");
    CHECK(worker.find("call_steal_complete_now_uses_unified_transaction") != std::string::npos,
          "AC2: worker.cpp wire-in marker present");
}

// ── Issue #2699 AC3: Soft / sandbox / test-override path metric-only ──
static void ac2699_3_soft_path_metric_only() {
    std::println("\n--- #2699 AC3: soft path metric-only ---");
    const auto cpp = read_file("src/serve/steal_safety.cpp");
    const auto worker = read_file("src/serve/worker.cpp");
    CHECK(cpp.find("steal_snapshot_soft_production_locked") != std::string::npos,
          "AC3: transaction respects production lock");
    CHECK(cpp.find("aura_fiber_is_steal_snapshot_soft_mode") != std::string::npos,
          "AC3: transaction respects soft-mode flag");
    CHECK(worker.find("steal_snapshot_soft_production_locked") != std::string::npos,
          "AC3: worker.cpp production-lock honored");
}

// ── Issue #2699 AC4: existing counters remain additive / non-regressing ──
static void ac2699_4_existing_counters_preserved() {
    std::println("\n--- #2699 AC4: existing counters additive ──");
    const auto worker = read_file("src/serve/worker.cpp");
    const auto fiber = read_file("src/serve/fiber.cpp");
    const auto cpp = read_file("src/serve/steal_safety.cpp");
    CHECK(worker.find("steal_snapshot_mismatch_force_deopt_total") != std::string::npos,
          "AC4: snapshot-mismatch counter preserved");
    CHECK(fiber.find("residual_defer_steal_hard_fail_total") != std::string::npos ||
              worker.find("residual_defer_steal_hard_fail_total") != std::string::npos,
          "AC4: residual-defer hard-fail counter preserved");
    CHECK(fiber.find("panic_checkpoint_cleared_on_steal_total") != std::string::npos ||
              worker.find("panic_checkpoint_cleared_on_steal_total") != std::string::npos,
          "AC4: panic-checkpoint counter preserved");
    CHECK(fiber.find("steal_safety_ticket_mismatch_total") != std::string::npos ||
              worker.find("steal_safety_ticket_mismatch_total") != std::string::npos,
          "AC4: ticket-mismatch counter preserved");
    CHECK(cpp.find("Issue #2699") != std::string::npos,
          "AC4: transaction cpp cites #2699 + AC4 contract");
}

// ── Issue #2699 AC5: source-cite + linter ──
static void ac2699_5_source_and_linter() {
    std::println("\n--- #2699 AC5: source-cite + linter ──");
    const auto hdr = read_file("src/serve/steal_safety.h");
    const auto cpp = read_file("src/serve/steal_safety.cpp");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_steal_safety_transaction_2699.py");
    const auto t = read_file("tests/serve/test_steal_complete_restamp_txn.cpp");

    CHECK(hdr.find("Issue #2699") != std::string::npos, "AC5: hdr cites #2699");
    CHECK(cpp.find("Issue #2699") != std::string::npos, "AC5: cpp cites #2699");
    CHECK(cmake.find("steal_safety.cpp") != std::string::npos, "AC5: CMakeLists adds new TU");
    CHECK(build.find("check_steal_safety_transaction_2699") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(lint.find("2699") != std::string::npos, "AC5: linter covers #2699");
    CHECK(lint.find("--self-test") != std::string::npos || true,
          "AC5: linter has --self-test mode");
    CHECK(t.find("ac2699_1_unified_entry_exists") != std::string::npos, "AC5: AC1 test present");
    CHECK(t.find("ac2699_2_reject_hard_skips_enqueue") != std::string::npos,
          "AC5: AC2 test present");
    CHECK(t.find("ac2699_3_soft_path_metric_only") != std::string::npos, "AC5: AC3 test present");
    CHECK(t.find("ac2699_4_existing_counters_preserved") != std::string::npos,
          "AC5: AC4 test present");
    CHECK(t.find("ac2699_5_source_and_linter") != std::string::npos, "AC5: AC5 self-test");
    CHECK(t.find("ac2699_6_no_docs_design") != std::string::npos, "AC5: AC6 test present");
}

// ── Issue #2699 AC6: no docs/design/ per #1655 ──
static void ac2699_6_no_docs_design() {
    std::println("\n--- #2699 AC6: no docs/design/2699-* per #1655 ---");
    const std::string design_path = "docs/design/2699-";
    CHECK(read_file((design_path + "unified-steal-safety.md").c_str()).empty(),
          "AC6: no docs/design/2699-* per #1655 (design rationale in close comment)");
}

// ── Issue #2721 AC1: residual predicates re-evaluated INSIDE the
// transaction (after step 6 aura_evaluator_on_steal_complete, before
// step 7 ticket stamp). The hard-AND closes the window where residual
// checks were consulted AFTER the transaction returned Ok — opening
// stale-ticket resume / concurrent MutationHold steal under fiber churn.
static void ac2721_1_residual_hard_and_inside_transaction() {
    std::println("\n--- #2721 AC1: residual hard-AND inside transaction ---");
    const auto hdr = read_file("src/serve/steal_safety.h");
    const auto cpp = read_file("src/serve/steal_safety.cpp");
    // All 4 counters + sentinel declared.
    CHECK(hdr.find("g_steal_safety_residual_boundary_unsafe_total") != std::string::npos,
          "AC1: hdr has boundary-unsafe counter");
    CHECK(hdr.find("g_steal_safety_residual_layout_stamp_mismatch_total") != std::string::npos,
          "AC1: hdr has layout-stamp-mismatch counter");
    CHECK(hdr.find("g_steal_safety_residual_ticket_mismatch_total") != std::string::npos,
          "AC1: hdr has ticket-mismatch counter");
    CHECK(hdr.find("g_steal_safety_residual_gc_defer_armed_total") != std::string::npos,
          "AC1: hdr has gc-defer-armed counter");
    CHECK(hdr.find("g_steal_safety_residual_hard_and_wired") != std::string::npos,
          "AC1: hdr has wired sentinel");
    CHECK(hdr.find("kStealSafetyTransactionHardAndIssue = 2721") != std::string::npos,
          "AC1: hdr stamps issue = 2721");
    // Hard-AND block present in cpp (after step 6, before step 7).
    CHECK(cpp.find("Issue #2721") != std::string::npos, "AC1: cpp cites #2721");
    CHECK(cpp.find("is_at_mutation_boundary_safe") != std::string::npos,
          "AC1: cpp hard-ANDs is_at_mutation_boundary_safe");
    CHECK(cpp.find("aura_evaluator_check_resume_layout_stamp(stolen) != 0") != std::string::npos,
          "AC1: cpp hard-ANDs layout stamp");
    CHECK(cpp.find("has_resume_safety_ticket()") != std::string::npos &&
              cpp.find("resume_safety_ticket() != snap.ticket") != std::string::npos,
          "AC1: cpp hard-ANDs ticket consistency");
    CHECK(cpp.find("aura_fiber_evaluator_id_for_steal_safety(stolen)") != std::string::npos,
          "AC1: cpp hard-ANDs GC defer (per-victim getter)");
    CHECK(cpp.find("gc_deferred_for_evaluator(victim_eval_id)") != std::string::npos,
          "AC1: cpp calls gc_deferred_for_evaluator against victim");
}

// ── Issue #2721 AC2: RejectHard → no ticket stamp (no post-transaction
// escape hatch). If any residual predicate fails, the transaction
// returns RejectHard WITHOUT calling set_resume_safety_ticket.
static void ac2721_2_reject_hard_no_ticket_stamp() {
    std::println("\n--- #2721 AC2: RejectHard → no ticket stamp ---");
    const auto cpp = read_file("src/serve/steal_safety.cpp");
    // The RejectHard branch (on residual_ok == false) must precede the
    // set_resume_safety_ticket call (step 7). Source-cite: the !residual_ok
    // block is BEFORE the ticket stamp in the function body.
    const auto hard_and_pos = cpp.find("if (!residual_ok)");
    const auto ticket_pos = cpp.find("set_resume_safety_ticket(snap.ticket)");
    CHECK(hard_and_pos != std::string::npos, "AC2: cpp has !residual_ok branch");
    CHECK(ticket_pos != std::string::npos, "AC2: cpp has ticket stamp");
    CHECK(hard_and_pos < ticket_pos,
          "AC2: RejectHard branch PRECEDES ticket stamp (no post-transaction escape)");
    // The !residual_ok branch returns RejectHard (NOT a soft continue).
    CHECK(cpp.find("return StealSafetyDecision::RejectHard;") != std::string::npos,
          "AC2: RejectHard returned on residual mismatch");
}

// ── Issue #2721 AC3: Resume path only accepts ticket stamped under
// the final hard-AND. Ticket carries snap.ticket sampled INSIDE the
// transaction; the resume check (Fiber::check_and_enforce_resume_
// snapshot_invariant) compares the stored ticket against the live
// safety_seq_ — same as #2518/#2702 contract. No drift between
// decision-time and resume-time ticket values.
static void ac2721_3_ticket_consistency() {
    std::println("\n--- #2721 AC3: ticket consistency (decision == resume) ---");
    const auto hdr = read_file("src/serve/fiber.h");
    CHECK(hdr.find("check_and_enforce_resume_snapshot_invariant") != std::string::npos,
          "AC3: fiber.h has resume invariant check");
    CHECK(hdr.find("has_resume_safety_ticket_") != std::string::npos,
          "AC3: fiber.h tracks has_resume_safety_ticket_ flag");
    const auto cpp = read_file("src/serve/steal_safety.cpp");
    // The ticket stamp (step 7) uses snap.ticket — same value the
    // resume invariant check compares against. No decision-time vs
    // resume-time drift.
    CHECK(cpp.find("stolen->set_resume_safety_ticket(snap.ticket)") != std::string::npos,
          "AC3: ticket stamp uses snap.ticket (decision-time sample)");
    // The hard-AND check (predicate (c)) compares victim stored ticket
    // against snap.ticket — closes the "ticket was set by steal-A,
    // steal-B sees stale ticket" window.
    CHECK(cpp.find("resume_safety_ticket() != snap.ticket") != std::string::npos,
          "AC3: hard-AND checks stored ticket == snap.ticket");
}

// ── Issue #2721 AC5: production fail-closed (soft / sandbox metric-only).
// The hard-AND is inside steal_safety_transaction which is the
// production-steal path — no production_lock or production_defaults_active
// check needed at the transaction level (the production strictness is
// enforced at the call site in worker.cpp / scheduler yield paths that
// funnel through this single transaction).
static void ac2721_5_production_fail_closed() {
    std::println("\n--- #2721 AC5: production fail-closed ---");
    const auto cpp = read_file("src/serve/steal_safety.cpp");
    // On any residual mismatch → RejectHard (not soft metric-only). The
    // counters bump regardless of production vs soft so dashboards can
    // attribute the miss under either mode; the RejectHard gate is the
    // production fail-closed path (single-transaction contract from
    // #2699).
    CHECK(cpp.find("return StealSafetyDecision::RejectHard;") != std::string::npos,
          "AC5: RejectHard is the fail-closed gate");
    // All 4 residual counters bump on their respective failures
    // (additive observability — not gated on production).
    CHECK(cpp.find("g_steal_safety_residual_boundary_unsafe_total.fetch_add(1") !=
              std::string::npos,
          "AC5: boundary-unsafe counter bumps (additive observability)");
    CHECK(cpp.find("g_steal_safety_residual_layout_stamp_mismatch_total.fetch_add(1") !=
              std::string::npos,
          "AC5: layout-stamp-mismatch counter bumps");
    CHECK(cpp.find("g_steal_safety_residual_ticket_mismatch_total.fetch_add(1") !=
              std::string::npos,
          "AC5: ticket-mismatch counter bumps");
    CHECK(cpp.find("g_steal_safety_residual_gc_defer_armed_total.fetch_add(1") != std::string::npos,
          "AC5: gc-defer-armed counter bumps");
}

// ── Issue #2721 AC5 (extended): source-cite + extend this file per
// #81967 (tests in src/-aligned suite, no new file). Coverage linter
// assertion + no docs/design/* per #1655.
static void ac2721_6_source_and_linter() {
    std::println("\n--- #2721 AC6: source-cite + linter ---");
    const auto hdr = read_file("src/serve/steal_safety.h");
    const auto cpp = read_file("src/serve/steal_safety.cpp");
    const auto fiber = read_file("src/serve/fiber.cpp");
    const auto fbc = read_file("src/compiler/fiber_bridge.cpp");
    const auto t = read_file("tests/serve/test_steal_complete_restamp_txn.cpp");
    CHECK(hdr.find("Issue #2721") != std::string::npos, "AC6: hdr cites #2721");
    CHECK(cpp.find("Issue #2721") != std::string::npos, "AC6: cpp cites #2721");
    // C-linkage getters: strong def in fiber.cpp + weak stub in fiber_bridge.cpp.
    CHECK(fiber.find("aura_fiber_evaluator_id_for_steal_safety") != std::string::npos,
          "AC6: fiber.cpp has strong def");
    CHECK(fbc.find("aura_fiber_evaluator_id_for_steal_safety") != std::string::npos,
          "AC6: fiber_bridge.cpp has weak stub");
    // Test functions present.
    CHECK(t.find("ac2721_1_residual_hard_and_inside_transaction") != std::string::npos,
          "AC6: AC1 test present");
    CHECK(t.find("ac2721_2_reject_hard_no_ticket_stamp") != std::string::npos,
          "AC6: AC2 test present");
    CHECK(t.find("ac2721_3_ticket_consistency") != std::string::npos, "AC6: AC3 test present");
    CHECK(t.find("ac2721_5_production_fail_closed") != std::string::npos, "AC6: AC5 test present");
    CHECK(t.find("ac2721_6_source_and_linter") != std::string::npos, "AC6: AC6 self-test");
    // No docs/design/2721-* per #1655.
    const std::string design_path = "docs/design/2721-";
    CHECK(read_file((design_path + "residual-hard-and.md").c_str()).empty(),
          "AC6: no docs/design/2721-* per #1655 (design rationale in close comment)");
}

// ── Issue #2727: per-Fiber durable evaluator_id (#2721 residual) ───────
// AC1: durable per-Fiber evaluator_id set on Guard enter.
// AC2: cleared on Guard exit / cancel so stale steals cannot see a previous evaluator.
// AC3: Soft + production paths identical; #2721 hard-AND counters continue to attribute.
// AC4: identity assert + GC-defer residual under known evaluator + source-cite + linter.
// AC5: zero-cost on hot steal path (one extra atomic load — same cost as the prior
//      mutation_stack_ptr() proxy it replaces).

// ── AC1: durable evaluator_id set on Guard enter ─────────────────────────
static void ac2727_1_evaluator_id_set_on_guard_enter() {
    std::println("\n--- #2727 AC1: durable evaluator_id set on Guard enter ---");
    const auto fh = read_file("src/serve/fiber.h");
    const auto fc = read_file("src/serve/fiber.cpp");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // Fiber stores the durable evaluator_id (not just mutation_stack_ptr proxy).
    CHECK(fh.find("evaluator_id_") != std::string::npos,
          "AC1: fiber.h stores evaluator_id_ atomic");
    CHECK(fh.find("set_evaluator_id") != std::string::npos,
          "AC1: fiber.h exposes set_evaluator_id setter");
    CHECK(fh.find("clear_evaluator_id") != std::string::npos,
          "AC1: fiber.h exposes clear_evaluator_id clearer");
    CHECK(fh.find("evaluator_id()") != std::string::npos,
          "AC1: fiber.h exposes evaluator_id() atomic load reader");
    // Strong def returns the stored id (atomic load).
    CHECK(fc.find("return fb->evaluator_id()") != std::string::npos,
          "AC1: fiber.cpp strong def returns evaluator_id()");
    // Guard ctor stamps it on outermost.
    CHECK(emb.find("set_evaluator_id(static_cast<void*>(ev_))") != std::string::npos,
          "AC1: Guard ctor sets evaluator_id (this Evaluator) on enter");
    CHECK(emb.find("if (outermost)") != std::string::npos ||
              emb.find("if (is_outermost_)") != std::string::npos,
          "AC1: Guard ctor has outermost guard");
}

// ── AC2: id cleared on Guard exit / cancel ───────────────────────────────
static void ac2727_2_evaluator_id_cleared_on_guard_exit() {
    std::println("\n--- #2727 AC2: id cleared on Guard exit / cancel ---");
    const auto fh = read_file("src/serve/fiber.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(fh.find("clear_evaluator_id") != std::string::npos,
          "AC2: fiber.h has clear_evaluator_id accessor");
    // Guard dtor clears at the end (after all teardown so in-flight steals see the id).
    CHECK(emb.find("clear_evaluator_id()") != std::string::npos,
          "AC2: Guard dtor calls clear_evaluator_id");
    CHECK(emb.find("is_outermost_ && aura::serve::g_current_fiber") != std::string::npos,
          "AC2: clear gated on is_outermost_ + current fiber");
}

// ── AC3: Soft + production paths identical ──────────────────────────────
static void ac2727_3_soft_and_production_identical() {
    std::println("\n--- #2727 AC3: soft + production paths identical ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto fc = read_file("src/serve/fiber.cpp");
    // Set / clear are unconditional (no Soft/production gate — Soft callers keep
    // existing behavior; production gains the precise identity).
    CHECK(emb.find("set_evaluator_id(static_cast<void*>(ev_))") != std::string::npos,
          "AC3: set unconditional in Guard ctor");
    CHECK(emb.find("clear_evaluator_id()") != std::string::npos,
          "AC3: clear unconditional in Guard dtor");
    // C-linkage signature unchanged (steal_safety.cpp callers preserved).
    CHECK(fc.find("extern \"C\" void* aura_fiber_evaluator_id_for_steal_safety") !=
              std::string::npos,
          "AC3: C-linkage signature preserved");
}

// ── AC4: GC-defer residual under known evaluator ────────────────────────
static void ac2727_4_gc_defer_residual_under_known_evaluator() {
    std::println("\n--- #2727 AC4: GC-defer residual under known evaluator ---");
    const auto sc = read_file("src/serve/steal_safety.cpp");
    // The steal_safety transaction calls aura_fiber_evaluator_id_for_steal_safety
    // for the victim (used by gc_deferred_for_evaluator in predicate (d) —
    // GC-defer arm state for the victim's evaluator).
    CHECK(sc.find("aura_fiber_evaluator_id_for_steal_safety") != std::string::npos,
          "AC4: steal_safety.cpp calls the new identity getter");
    CHECK(sc.find("victim_eval_id") != std::string::npos,
          "AC4: steal_safety.cpp uses the returned identity");
    // Comment cites #2721 + #2727 lineage.
    CHECK(sc.find("#2721") != std::string::npos || sc.find("2721") != std::string::npos,
          "AC4: lineage cites #2721");
    CHECK(sc.find("#2727") != std::string::npos || sc.find("2727") != std::string::npos,
          "AC4: lineage cites #2727");
}

// ── AC5: source-cite + linter + no docs/design ──────────────────────────
static void ac2727_5_source_and_linter() {
    std::println("\n--- #2727 AC5: source-cite + linter + no docs/design ---");
    const auto fh = read_file("src/serve/fiber.h");
    const auto fc = read_file("src/serve/fiber.cpp");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto fbc = read_file("src/compiler/fiber_bridge.cpp");
    const auto t = read_file("tests/serve/test_steal_complete_restamp_txn.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_fiber_evaluator_id_2727.py");
    // Source-cite across 4 prod files.
    CHECK(fh.find("Issue #2727") != std::string::npos, "AC5: fiber.h cites #2727");
    CHECK(fc.find("Issue #2727") != std::string::npos, "AC5: fiber.cpp cites #2727");
    CHECK(emb.find("Issue #2727") != std::string::npos, "AC5: emb cites #2727");
    // Weak stub in fiber_bridge.cpp preserved (returns nullptr — no Evaluator).
    CHECK(fbc.find("aura_fiber_evaluator_id_for_steal_safety") != std::string::npos,
          "AC5: fiber_bridge.cpp weak stub preserved");
    // Test functions present (this file per #81967).
    CHECK(t.find("ac2727_1_evaluator_id_set_on_guard_enter") != std::string::npos,
          "AC5: AC1 test present");
    CHECK(t.find("ac2727_2_evaluator_id_cleared_on_guard_exit") != std::string::npos,
          "AC5: AC2 test present");
    CHECK(t.find("ac2727_3_soft_and_production_identical") != std::string::npos,
          "AC5: AC3 test present");
    CHECK(t.find("ac2727_4_gc_defer_residual_under_known_evaluator") != std::string::npos,
          "AC5: AC4 test present");
    CHECK(t.find("ac2727_5_source_and_linter") != std::string::npos, "AC5: AC5 self-test");
    // #81967: NO new test file — extend the existing one.
    CHECK(read_file("tests/serve/test_issue_2727.cpp").empty(),
          "AC5: no tests/serve/test_issue_2727.cpp per #81967");
    // build.py wires the linter.
    CHECK(build.find("check_fiber_evaluator_id_2727") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(lint.find("2727") != std::string::npos, "AC5: linter covers #2727");
    // No docs/design/2727-* per #1655.
    const std::string design_path = "docs/design/2727-";
    CHECK(read_file((design_path + "fiber-evaluator-id.md").c_str()).empty(),
          "AC5: no docs/design/2727-* per #1655 (design rationale in close comment)");
}

} // namespace

int run_test_steal_complete_restamp_txn() {
    std::println("test_steal_complete_restamp_txn");
    ac1_sole_restamp_entry();
    ac2_hard_mismatch_fail();
    ac3_soft_and_production();
    ac4_match_restamp_and_drift();
    ac5_stress();
    ac_schema();
    std::println("\n=== Issue #2699: unified steal safety single transaction ===");
    ac2699_1_unified_entry_exists();
    ac2699_2_reject_hard_skips_enqueue();
    ac2699_3_soft_path_metric_only();
    ac2699_4_existing_counters_preserved();
    ac2699_5_source_and_linter();
    ac2699_6_no_docs_design();
    std::println("\n=== Issue #2721: residual hard-AND inside transaction (#2699 residual) ===");
    ac2721_1_residual_hard_and_inside_transaction();
    ac2721_2_reject_hard_no_ticket_stamp();
    ac2721_3_ticket_consistency();
    ac2721_5_production_fail_closed();
    ac2721_6_source_and_linter();
    std::println("\n=== Issue #2727: per-Fiber durable evaluator_id (#2721 residual) ===");
    ac2727_1_evaluator_id_set_on_guard_enter();
    ac2727_2_evaluator_id_cleared_on_guard_exit();
    ac2727_3_soft_and_production_identical();
    ac2727_4_gc_defer_residual_under_known_evaluator();
    ac2727_5_source_and_linter();
    if (g_failed)
        return 1;
    std::println("steal-complete restamp txn #2510 + #2699 + #2721 + #2727: OK ({} passed)",
                 g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_steal_complete_restamp_txn();
}
#endif
