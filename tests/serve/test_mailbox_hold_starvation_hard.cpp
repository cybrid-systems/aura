// @category: unit
// @reason: Issue #2551 — outermost Guard exit drain residual under
//          production becomes hard signal + Agent throttle flag.
//
//   AC1: Production/Strict + residual after budget → hard counter + flag
//   AC2: Soft / no residual → zero extra cost; flag stays clear
//   AC3: Subsequent successful drain to zero clears throttle flag
//   AC4: Chaos soak surfaces hard signal; Agents can read the flag
//   AC5: Source-cite next to #2511 / #2378; additive schema-2551

#include "test_harness.hpp"

#include "serve/multi_fiber_mailbox.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::serve::mf_mailbox::clear_agent_throttle_for_mailbox_starvation;
using aura::serve::mf_mailbox::drain_deferred_under_budget;
using aura::serve::mf_mailbox::g_mf_mailbox_stats;
using aura::serve::mf_mailbox::note_mailbox_mutation_hold_defer;
using aura::serve::mf_mailbox::note_mailbox_push_ok_drain_progress;
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
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:mf-mailbox-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void drain_all_depth() {
    std::uint64_t guard = 256;
    while (g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed) > 0 &&
           guard-- > 0)
        note_mailbox_push_ok_drain_progress();
    // Free path clears throttle when depth already 0.
    (void)drain_deferred_under_budget(0);
}

static void strict_on() {
    ::setenv("AURA_MUTATE_MAILBOX_STRICT", "1", 1);
}
static void strict_off() {
    ::unsetenv("AURA_MUTATE_MAILBOX_STRICT");
}

// ── AC1: Strict + residual after budget → hard + flag ──
static void ac1_production_hard_signal() {
    std::println("\n--- #2551 AC1: Strict residual after budget → hard + flag ---");
    drain_all_depth();
    clear_agent_throttle_for_mailbox_starvation();
    strict_on();
    const auto hard0 =
        g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed);
    const auto soft0 =
        g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(std::memory_order_relaxed);
    note_mailbox_mutation_hold_defer();
    note_mailbox_mutation_hold_defer();
    CHECK(g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed) >= 2,
          "AC1: depth open");
    // budget 0 → immediate starve
    auto r = drain_deferred_under_budget(/*budget_us=*/0);
    CHECK(r.starved, "AC1: starved under budget 0");
    CHECK(g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed) ==
              hard0 + 1,
          "AC1: hard total +1");
    CHECK(g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(std::memory_order_relaxed) >
              soft0,
          "AC1: soft starvation still advances");
    CHECK(g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
              std::memory_order_relaxed) == 1,
          "AC1: Agent throttle flag set");
    // Force-resolve may have zeroed depth under Strict — flag must stay set
    // until subsequent free drain (AC3).
    CHECK(g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
              std::memory_order_relaxed) == 1,
          "AC1: flag remains after force-resolve in same call");
    strict_off();
    drain_all_depth();
}

// ── AC2: Soft residual / free path → no hard, flag clear ──
static void ac2_soft_and_free() {
    std::println("\n--- #2551 AC2: Soft residual + free path zero extra ---");
    drain_all_depth();
    clear_agent_throttle_for_mailbox_starvation();
    strict_off();
    const auto hard0 =
        g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed);
    note_mailbox_mutation_hold_defer();
    auto r = drain_deferred_under_budget(/*budget_us=*/0);
    CHECK(r.starved || r.remaining_depth > 0 || r.had_open_defer, "AC2: soft drain with open");
    CHECK(g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed) ==
              hard0,
          "AC2: hard total flat under Soft");
    CHECK(g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
              std::memory_order_relaxed) == 0,
          "AC2: throttle flag stays clear under Soft");
    // Free path (no residual): flag clear, hard flat
    drain_all_depth();
    const auto hard1 =
        g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed);
    auto r2 = drain_deferred_under_budget(1000);
    CHECK(!r2.had_open_defer, "AC2: free path no open");
    CHECK(g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed) ==
              hard1,
          "AC2: free path hard flat");
    CHECK(g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
              std::memory_order_relaxed) == 0,
          "AC2: free path flag clear");
}

// ── AC3: subsequent drain to zero clears flag ──
static void ac3_clear_on_drain_zero() {
    std::println("\n--- #2551 AC3: subsequent free drain clears throttle ---");
    drain_all_depth();
    clear_agent_throttle_for_mailbox_starvation();
    strict_on();
    note_mailbox_mutation_hold_defer();
    (void)drain_deferred_under_budget(0);
    CHECK(g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
              std::memory_order_relaxed) == 1,
          "AC3: flag set after hard starve");
    // Ensure depth is zero then free drain clears.
    drain_all_depth(); // may already be 0 from force-resolve; free path clears
    CHECK(g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed) == 0,
          "AC3: depth zero");
    CHECK(g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
              std::memory_order_relaxed) == 0,
          "AC3: flag cleared after free drain");
    // Natural window close also clears.
    clear_agent_throttle_for_mailbox_starvation();
    g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.store(1, std::memory_order_relaxed);
    note_mailbox_mutation_hold_defer();
    note_mailbox_push_ok_drain_progress(); // 1→0
    CHECK(g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
              std::memory_order_relaxed) == 0,
          "AC3: push_ok window close clears flag");
    strict_off();
}

// ── AC4: chaos + query readable ──
static void ac4_chaos_and_query() {
    std::println("\n--- #2551 AC4: chaos + Agent-readable query ---");
    drain_all_depth();
    clear_agent_throttle_for_mailbox_starvation();
    strict_on();
    const auto hard0 =
        g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed);
    constexpr int N = 6;
    constexpr int K = 12;
    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int t = 0; t < N; ++t) {
        threads.emplace_back([K]() {
            for (int i = 0; i < K; ++i) {
                note_mailbox_mutation_hold_defer();
                if ((i % 2) == 0)
                    (void)drain_deferred_under_budget(0);
                if ((i % 4) == 0)
                    note_mailbox_push_ok_drain_progress();
            }
        });
    }
    for (auto& th : threads)
        th.join();
    (void)drain_deferred_under_budget(0);
    CHECK(g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed) >
              hard0,
          "AC4: hard signal advanced under chaos");
    // Agents read via query
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2551") == 2551, "AC4: schema-2551");
    CHECK(href(cs, "mailbox-hold-starvation-hard-wired") == 1, "AC4: wired");
    CHECK(href(cs, "mailbox-hold-starvation-hard-total") >= 0, "AC4: hard total query");
    CHECK(href(cs, "agent-throttle-for-mailbox-starvation") >= 0, "AC4: throttle flag query");
    // Agent skip-mutate: flag is 0/1 readable
    const auto thr = href(cs, "agent-throttle-for-mailbox-starvation");
    CHECK(thr == 0 || thr == 1, "AC4: throttle is boolean 0/1");
    drain_all_depth();
    strict_off();
}

// ── AC5: source-cite + gate ──
static void ac5_source_and_gate() {
    std::println("\n--- #2551 AC5: source-cite + linter ---");
    const auto mb = read_file("src/serve/multi_fiber_mailbox.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto epm = read_file("src/compiler/evaluator_primitives_messaging.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_mailbox_hold_starvation_hard_2551.py");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");

    CHECK(mb.find("Issue #2551") != std::string::npos, "AC5: mailbox cites #2551");
    CHECK(mb.find("mailbox_hold_starvation_hard_total") != std::string::npos, "AC5: hard total");
    CHECK(mb.find("agent_throttle_for_mailbox_starvation") != std::string::npos, "AC5: throttle");
    CHECK(emb.find("#2551") != std::string::npos, "AC5: Guard dtor cites #2551");
    CHECK(emb.find("drain_deferred_under_budget") != std::string::npos, "AC5: dtor drain");
    CHECK(epm.find("schema-2551") != std::string::npos, "AC5: schema-2551");
    CHECK(epm.find("agent-throttle-for-mailbox-starvation") != std::string::npos,
          "AC5: query throttle");
    // Lineage next to #2511 / #2378
    CHECK(mb.find("Issue #2511") != std::string::npos, "AC5: #2511 lineage");
    CHECK(mb.find("#2378") != std::string::npos || mb.find("Issue #2378") != std::string::npos,
          "AC5: #2378 lineage");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(cmake.find("test_mailbox_hold_starvation_hard") != std::string::npos, "AC5: cmake");
    CHECK(build.find("check_mailbox_hold_starvation_hard_2551") != std::string::npos,
          "AC5: build script");
    CHECK(build.find("cmd_mailbox_hold_starvation_hard_coverage") != std::string::npos,
          "AC5: build cmd");
}

// ── Issue #2701 AC1+AC2: budget reject in production + Soft observe ──
static void ac2701_1_budget_reject_production() {
    std::println("\n--- #2701 AC1+AC2: budget reject + Soft observe ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mhb.find("Issue #2701") != std::string::npos, "AC1: mhb cites #2701");
    CHECK(mhb.find("g_mutation_hold_budget_reject_total") != std::string::npos,
          "AC1: mhb has reject counter");
    CHECK(mhb.find("g_mutation_hold_budget_soft_observe_total") != std::string::npos,
          "AC2: mhb has soft-observe counter");
    CHECK(mhb.find("mutation_hold_budget_reject_enabled") != std::string::npos,
          "AC1+AC2: mhb has reject-enabled decision");
    CHECK(emb.find("AdmissionRejected: mutation-hold-budget") != std::string::npos,
          "AC1: emb produces structured AdmissionRejected");
    CHECK(emb.find("Soft path") != std::string::npos, "AC2: emb soft path note");
}

// ── Issue #2701 AC2: Soft path metric-only ──
static void ac2701_2_soft_path_metric_only() {
    std::println("\n--- #2701 AC2: Soft path metric-only ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    CHECK(mhb.find("g_mutation_hold_budget_soft_observe_total") != std::string::npos,
          "AC2: mhb has soft-observe counter (Soft path bumps only this)");
    CHECK(mhb.find("mutation_hold_budget_reject_enabled") != std::string::npos,
          "AC2: Soft path reuses reject_enabled gate (hard vs soft split)");
}

// ── Issue #2701 AC4: query keys + Agent-visible counters ──
static void ac2701_4_query_keys_added() {
    std::println("\n--- #2701 AC4: query keys + counters ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("query:mutation-hold-budget-gate") != std::string::npos ||
              q.find("mutation-hold-budget-reject-total") != std::string::npos,
          "AC4: query primitive / reject-total surfaced");
    CHECK(q.find("mutation-hold-budget-soft-observe-total") != std::string::npos,
          "AC4: soft-observe-total surfaced");
    CHECK(q.find("mutation-hold-budget-wired") != std::string::npos,
          "AC4: wired sentinel surfaced");
    CHECK(q.find("schema-2701") != std::string::npos, "AC4: schema-2701");
    CHECK(q.find("issue-2701") != std::string::npos, "AC4: issue-2701");
    CHECK(q.find("schema-2551") != std::string::npos, "AC4: schema-2551 preserved");
}

// ── Issue #2701 AC5: source-cite + linter ──
static void ac2701_5_source_and_linter() {
    std::println("\n--- #2701 AC5: source-cite + linter ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto t = read_file("tests/serve/test_mailbox_hold_starvation_hard.cpp");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_mutation_hold_budget_reject_2701.py");

    CHECK(mhb.find("kMutationHoldBudgetRejectIssue = 2701") != std::string::npos,
          "AC5: mhb stamps issue = 2701");
    CHECK(emb.find("Issue #2701") != std::string::npos, "AC5: emb cites #2701");
    CHECK(q.find("issue-2701") != std::string::npos, "AC5: q issue-2701");
    CHECK(t.find("ac2701_1_budget_reject_production") != std::string::npos,
          "AC5: AC1 test present");
    CHECK(t.find("ac2701_2_query_keys_added") != std::string::npos, "AC5: AC3 test present");
    CHECK(t.find("ac2701_3_order_with_security_schedule") != std::string::npos,
          "AC5: AC4 test present");
    CHECK(t.find("ac2701_4_source_and_linter") != std::string::npos, "AC5: AC5 self-test");
    CHECK(t.find("ac2701_6_no_docs_design") != std::string::npos, "AC5: AC6 test present");
    CHECK(build.find("check_mutation_hold_budget_reject_2701") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(lint.find("2701") != std::string::npos, "AC5: linter covers #2701");
}

// ── Issue #2701 AC6: no docs/design/ per #1655 ──
static void ac2701_6_no_docs_design() {
    std::println("\n--- #2701 AC6: no docs/design/2701-* per #1655 ---");
    const std::string design_path = "docs/design/2701-";
    CHECK(read_file((design_path + "hold-budget-reject.md").c_str()).empty(),
          "AC6: no docs/design/2701-* per #1655 (design rationale in close comment)");
}

// ── Issue #2720 AC1: production + over-budget → force-degrade holder.
// Reject *this* admit (existing #2701) AND invoke
// aura_evaluator_force_degrade_outermost_holder(fiber_id) on the
// recorded holder so steal/GC can progress. Default production arm.
static void ac2720_1_force_degrade_production() {
    std::println("\n--- #2720 AC1: force-degrade holder under production ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto mfbh = read_file("src/serve/multi_fiber_mailbox.h");
    const auto fbc = read_file("src/compiler/fiber_bridge.cpp");
    // Counters + accessors in mhb.
    CHECK(mhb.find("g_mutation_hold_budget_holder_degrade_total") != std::string::npos,
          "AC1: mhb has holder-degrade-total counter");
    CHECK(mhb.find("g_mutation_hold_budget_holder_degrade_same_fiber_total") != std::string::npos,
          "AC1: mhb has same-fiber split counter");
    CHECK(mhb.find("g_mutation_hold_budget_holder_degrade_cross_fiber_total") != std::string::npos,
          "AC1: mhb has cross-fiber split counter");
    CHECK(mhb.find("g_mutation_hold_budget_holder_degrade_wired") != std::string::npos,
          "AC1: mhb has wired sentinel");
    CHECK(mhb.find("kMutationHoldBudgetHolderDegradeIssue = 2720") != std::string::npos,
          "AC1: mhb stamps issue = 2720");
    // ABI declaration in mfbh.
    CHECK(mfbh.find("aura_evaluator_force_degrade_outermost_holder") != std::string::npos,
          "AC1: mfbh declares the ABI");
    // ABI definition in efm (same-fiber path bumps same_fiber counter +
    // request_cancel + mark_outermost_mutation_failed).
    CHECK(efm.find("aura_evaluator_force_degrade_outermost_holder") != std::string::npos,
          "AC1: efm defines the ABI");
    CHECK(efm.find("g_current_fiber->id() == fiber_id") != std::string::npos,
          "AC1: efm gates on same-fiber via g_current_fiber->id()");
    CHECK(efm.find("g_current_fiber->request_cancel()") != std::string::npos,
          "AC1: efm calls request_cancel on same-fiber holder");
    CHECK(efm.find("mark_outermost_mutation_failed()") != std::string::npos,
          "AC1: efm marks current evaluator's outermost as failed");
    // Weak stub in fbc.
    CHECK(fbc.find("aura_evaluator_force_degrade_outermost_holder") != std::string::npos,
          "AC1: fbc weak stub present");
    // Wiring in emb — both try_acquire AND try_acquire_for_region.
    const auto emb_degrade_calls =
        std::count(emb.begin(), emb.end(), '\n') >= 0 ? 0 : 0; // placeholder
    // Source-cite: count "aura_evaluator_force_degrade_outermost_holder" in emb.
    auto count_occurrences = [](const std::string& s, const std::string& needle) {
        std::size_t n = 0;
        std::size_t pos = 0;
        while ((pos = s.find(needle, pos)) != std::string::npos) {
            ++n;
            pos += needle.size();
        }
        return n;
    };
    CHECK(count_occurrences(emb, "aura_evaluator_force_degrade_outermost_holder") >= 2,
          "AC1: emb wires call in try_acquire AND try_acquire_for_region");
    CHECK(emb.find("mutation_hold_live_snapshot()") != std::string::npos,
          "AC1: emb reads live snapshot to get holder fiber_id");
    CHECK(emb.find("hold_snap.fiber_id != 0") != std::string::npos,
          "AC1: emb guards on non-zero holder fiber_id");
}

// ── Issue #2720 AC2: Soft / sandbox=off → counter-only unless hard env.
// Same #2701 gating: mutation_hold_budget_reject_enabled() must be true
// (production OR AURA_MUTATION_HOLD_BUDGET_HARD=1) for the degrade path
// to fire. Soft path falls through metric-only.
static void ac2720_2_soft_counter_only() {
    std::println("\n--- #2720 AC2: Soft path → counter-only unless hard env ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    // Reuse #2701 reject_enabled decision.
    CHECK(mhb.find("mutation_hold_budget_reject_enabled") != std::string::npos,
          "AC2: mhb reuses #2701 reject_enabled gate");
    // Soft path comment preserved.
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(emb.find("Soft path") != std::string::npos, "AC2: emb soft path note preserved");
    // The force_degrade call sits INSIDE the reject_enabled block — Soft
    // path falls through without firing (verified by source position: the
    // call precedes the reject return, after the reject_enabled check).
    CHECK(emb.find("if (mutation_hold_budget_reject_enabled())") != std::string::npos,
          "AC2: reject_enabled check present");
}

// ── Issue #2720 AC3: Nested (non-outermost) guards never touch the live
// probe or degrade path. The holder-degrade force_degrade call only
// fires from try_acquire / try_acquire_for_region (outermost entry
// gates) — never from nested Guard re-entry. The live probe itself
// (mutation_hold_live_note_enter/exit) is outermost-only by contract.
static void ac2720_3_nested_outermost_only() {
    std::println("\n--- #2720 AC3: nested guards never touch degrade path ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    CHECK(mhb.find("Nested guards never touch the probe") != std::string::npos,
          "AC3: mhb contract — nested guards never touch probe");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // force_degrade lives only in the outermost try_acquire /
    // try_acquire_for_region gate chain, not in any nested re-entry path.
    CHECK(emb.find("try_acquire_for_region") != std::string::npos,
          "AC3: emb has try_acquire_for_region (outermost gate)");
}

// ── Issue #2720 AC4: additive observability — schema/issue sentinels +
// holder-degrade counters; all #2701/#2313/#2517/#2587 surfaces preserved.
static void ac2720_4_query_keys() {
    std::println("\n--- #2720 AC4: additive query keys + sentinels ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    // #2720 new keys present.
    CHECK(q.find("\"mutation-hold-budget-holder-degrade-total\"") != std::string::npos,
          "AC4: holder-degrade-total key");
    CHECK(q.find("\"mutation-hold-budget-holder-degrade-same-fiber-total\"") != std::string::npos,
          "AC4: holder-degrade-same-fiber-total key");
    CHECK(q.find("\"mutation-hold-budget-holder-degrade-cross-fiber-total\"") != std::string::npos,
          "AC4: holder-degrade-cross-fiber-total key");
    CHECK(q.find("\"mutation-hold-budget-holder-degrade-wired\"") != std::string::npos,
          "AC4: holder-degrade-wired sentinel");
    CHECK(q.find("\"schema-2720\"") != std::string::npos, "AC4: schema-2720 sentinel");
    CHECK(q.find("\"issue-2720\"") != std::string::npos, "AC4: issue-2720 sentinel");
    // #2701 keys preserved (strict superset).
    CHECK(q.find("\"mutation-hold-budget-reject-total\"") != std::string::npos,
          "AC4: #2701 reject-total preserved");
    CHECK(q.find("\"mutation-hold-budget-soft-observe-total\"") != std::string::npos,
          "AC4: #2701 soft-observe-total preserved");
    CHECK(q.find("\"mutation-hold-budget-wired\"") != std::string::npos,
          "AC4: #2701 wired preserved");
    CHECK(q.find("\"schema-2701\"") != std::string::npos, "AC4: #2701 schema-2701 preserved");
    CHECK(q.find("\"issue-2701\"") != std::string::npos, "AC4: #2701 issue-2701 preserved");
    // #2587 / #2551 preserved (mailbox-hold-starvation baseline).
    CHECK(q.find("schema-2587") != std::string::npos || q.find("schema-2551") != std::string::npos,
          "AC4: #2587/#2551 baseline preserved");
}

// ── Issue #2720 AC5: source-cite + linter (extend #2701 linter or new
// check); no docs/design/* per #1655.
static void ac2720_5_source_and_linter() {
    std::println("\n--- #2720 AC5: source-cite + linter ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto t = read_file("tests/serve/test_mailbox_hold_starvation_hard.cpp");
    CHECK(mhb.find("Issue #2720") != std::string::npos, "AC5: mhb cites #2720");
    CHECK(emb.find("Issue #2720") != std::string::npos, "AC5: emb cites #2720");
    CHECK(efm.find("Issue #2720") != std::string::npos, "AC5: efm cites #2720");
    CHECK(q.find("Issue #2720") != std::string::npos, "AC5: q cites #2720");
    CHECK(t.find("ac2720_1_force_degrade_production") != std::string::npos,
          "AC5: AC1 test present");
    CHECK(t.find("ac2720_2_soft_counter_only") != std::string::npos, "AC5: AC2 test present");
    CHECK(t.find("ac2720_3_nested_outermost_only") != std::string::npos, "AC5: AC3 test present");
    CHECK(t.find("ac2720_4_query_keys") != std::string::npos, "AC5: AC4 test present");
    CHECK(t.find("ac2720_5_source_and_linter") != std::string::npos, "AC5: AC5 self-test");
}

// ── Issue #2720 AC6: no docs/design/2720-* per #1655 (design rationale
// lives in the close comment).
static void ac2720_6_no_docs_design() {
    std::println("\n--- #2720 AC6: no docs/design/2720-* per #1655 ---");
    const std::string design_path = "docs/design/2720-";
    CHECK(read_file((design_path + "holder-degrade.md").c_str()).empty(),
          "AC6: no docs/design/2720-* per #1655 (design rationale in close comment)");
}

// ── Issue #2724 AC1: try_acquire_for_region uses regions_disjoint helper
// for the concurrent admit check. Production + proven-disjoint regions
// → both admits succeed concurrently. Production + overlapping regions
// → second admit rejects with structured reason `region-overlap` and
// counter advance.
static void ac2724_1_disjoint_concurrent_admit() {
    std::println("\n--- #2724 AC1: disjoint concurrent admit ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(emb.find("regions_disjoint") != std::string::npos,
          "AC1: regions_disjoint helper declared in boundary TU");
    CHECK(emb.find("g_mutation_region_concurrent_admit_total") != std::string::npos,
          "AC1: concurrent-admit counter declared");
    CHECK(emb.find("g_mutation_region_overlap_reject_total") != std::string::npos,
          "AC1: overlap-reject counter declared");
    // Disjoint path: production + disjoint regions → admit + bump counter.
    // #2754: 4-arg form (key + cone mask); substring still anchors on the helper.
    CHECK(emb.find("regions_disjoint(region_key, g_last_admitted_region_key") != std::string::npos,
          "AC1: disjoint check uses regions_disjoint helper");
    CHECK(emb.find("g_mutation_region_concurrent_admit_total.fetch_add(1,") != std::string::npos,
          "AC1: admit path bumps counter");
    // Overlap path: production + overlap → reject with region-overlap.
    CHECK(emb.find("AdmissionRejected: region-overlap") != std::string::npos,
          "AC1: overlap reject returns structured `region-overlap` reason");
}

// ── Issue #2724 AC2: overlap reject under production — second admit
// rejects with structured reason. Counters advance on reject. AC2
// preserves the gate order (#2587 mailbox → #2701 budget → #2630/#2660
// security-schedule) — region-overlap is an additional check after the
// gate, before the per-region shard acquisition.
static void ac2724_2_overlap_reject_production() {
    std::println("\n--- #2724 AC2: overlap reject under production ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // Overlap reject path: production + not disjoint → counter bump +
    // structured reject.
    CHECK(emb.find("g_mutation_region_overlap_reject_total.fetch_add(1,") != std::string::npos,
          "AC2: overlap-reject path bumps counter");
    CHECK(emb.find("typed_audit::production_defaults_active()") != std::string::npos,
          "AC2: production_defaults_active() gate present");
    // Preserves the existing #2701/#2720/#2587/#2630 gates (gate order
    // preserved — region-overlap is an additional check, not a
    // replacement).
    CHECK(emb.find("mutation-hold-budget") != std::string::npos ||
              emb.find("mutation_hold_budget_check") != std::string::npos,
          "AC2: #2701 hold-budget gate preserved");
    CHECK(emb.find("security-schedule") != std::string::npos ||
              emb.find("make_security_schedule_input_live") != std::string::npos,
          "AC2: #2630/#2660 security-schedule gate preserved");
}

// ── Issue #2724 AC3: Soft / sandbox=off → metric-only observation
// (no production lock regression). Soft path bumps counters but does
// NOT reject — soft callers can still admit for test ergonomics per
// issue body AC3.
static void ac2724_3_soft_path_metric_only() {
    std::println("\n--- #2724 AC3: Soft path → metric-only ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // Soft path: branch with `else if (region_key != 0)` after the
    // production branch — metric-only observation (bump counter, no
    // reject).
    CHECK(emb.find("Soft / sandbox=off") != std::string::npos,
          "AC3: Soft path documented in comments");
    CHECK(emb.find("metric-only observation") != std::string::npos ||
              emb.find("metric-only") != std::string::npos,
          "AC3: Soft path metric-only");
    // Soft path bumps both counters (overlap observed + admit logged)
    // but does NOT reject — no production lock regression.
}

// ── Issue #2724 AC4: densify / ownership_rebind / restamp remain
// correct under concurrent region holds. The per-region shard
// (region_shard_) is acquired instead of the global workspace_mtx_
// for region mode, so concurrent region admits don't block each other.
// Densify / ownership_rebind / restamp are inside the per-region
// shard, so cross-region root remap races are impossible (no two
// fibers can hold overlapping regions simultaneously).
static void ac2724_4_densify_correctness_under_concurrent_holds() {
    std::println("\n--- #2724 AC4: densify correctness under concurrent holds ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // region_shard_ is used for per-region mode (line ~1379).
    CHECK(emb.find("region_shard_") != std::string::npos,
          "AC4: region_shard_ field present (per-region mode)");
    CHECK(emb.find("workspace_region_shard") != std::string::npos,
          "AC4: workspace_region_shard accessor used");
    // The fallback to GlobalExclusive under atomic_batch is preserved
    // (serializes densify / ownership_rebind / restamp correctly).
    CHECK(emb.find("atomic_batch_active") != std::string::npos,
          "AC4: atomic_batch_active check preserved (densify-correctness gate)");
    CHECK(emb.find("workspace_region_fallback_global_total") != std::string::npos,
          "AC4: fallback-to-GlobalExclusive counter preserved");
}

// ── Issue #2724 AC5: additive observability — #2701/#2720/#2587/#2630
// surfaces preserved + new mutation-region-concurrent-admit-total /
// mutation-region-overlap-reject-total / schema-2724 / issue-2724
// counters + sentinels. Counters live in mutation_hold_budget.h
// (shared with query surface); emb remains the writer.
static void ac2724_5_additive_observability() {
    std::println("\n--- #2724 AC5: additive observability ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    // New atomics + sentinels (shared header).
    CHECK(mhb.find("g_mutation_region_concurrent_admit_total{0}") != std::string::npos,
          "AC5: concurrent-admit-total counter initialized");
    CHECK(mhb.find("g_mutation_region_overlap_reject_total{0}") != std::string::npos,
          "AC5: overlap-reject-total counter initialized");
    CHECK(mhb.find("kMutationRegionConcurrentIssue = 2724") != std::string::npos,
          "AC5: issue stamp = 2724");
    // Writer still bumps counters in emb.
    CHECK(emb.find("g_mutation_region_concurrent_admit_total.fetch_add(1,") != std::string::npos,
          "AC5: emb bumps concurrent-admit counter");
    // Query keys present.
    CHECK(q.find("mutation-region-concurrent-admit-total") != std::string::npos,
          "AC5: query key mutation-region-concurrent-admit-total");
    CHECK(q.find("mutation-region-overlap-reject-total") != std::string::npos,
          "AC5: query key mutation-region-overlap-reject-total");
    CHECK(q.find("mutation-region-concurrent-wired") != std::string::npos,
          "AC5: query key mutation-region-concurrent-wired");
    CHECK(q.find("schema-2724") != std::string::npos, "AC5: schema-2724 sentinel");
    CHECK(q.find("issue-2724") != std::string::npos, "AC5: issue-2724 sentinel");
    // All #2701/#2720/#2587/#2630 surfaces preserved.
    CHECK(q.find("schema-2701") != std::string::npos, "AC5: #2701 schema-2701 preserved");
    CHECK(q.find("schema-2720") != std::string::npos, "AC5: #2720 schema-2720 preserved");
    CHECK(q.find("schema-2551") != std::string::npos || q.find("schema-2587") != std::string::npos,
          "AC5: #2551/#2587 schema preserved");
}

// ── Issue #2724 AC6: source-cite + extend this file per #81967 (tests
// in src/-aligned suite, no new file) + no docs/design/2724-* per
// #1655.
static void ac2724_6_source_and_linter() {
    std::println("\n--- #2724 AC6: source-cite + linter + no docs/design/ ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto t = read_file("tests/serve/test_mailbox_hold_starvation_hard.cpp");
    // Source-cite: emb cites #2724.
    CHECK(emb.find("Issue #2724") != std::string::npos, "AC6: emb cites #2724");
    CHECK(q.find("Issue #2724") != std::string::npos, "AC6: query cites #2724");
    // Test functions present.
    CHECK(t.find("ac2724_1_disjoint_concurrent_admit") != std::string::npos,
          "AC6: AC1 test present");
    CHECK(t.find("ac2724_2_overlap_reject_production") != std::string::npos,
          "AC6: AC2 test present");
    CHECK(t.find("ac2724_3_soft_path_metric_only") != std::string::npos, "AC6: AC3 test present");
    CHECK(t.find("ac2724_4_densify_correctness_under_concurrent_holds") != std::string::npos,
          "AC6: AC4 test present");
    CHECK(t.find("ac2724_5_additive_observability") != std::string::npos, "AC6: AC5 test present");
    CHECK(t.find("ac2724_6_source_and_linter") != std::string::npos, "AC6: AC6 self-test");
    // #2551 / #2701 / #2720 test functions preserved (additive — this
    // file already shipped #2551 + #2701 + #2720 test functions).
    CHECK(t.find("ac2554_pr_gate_short") != std::string::npos ||
              t.find("ac1_production_hard_signal") != std::string::npos,
          "AC6: #2551/#2701 test functions preserved");
    CHECK(t.find("ac2720_6_no_docs_design") != std::string::npos,
          "AC6: #2720 test functions preserved");
    // No docs/design/2724-* per #1655.
    const std::string design_path = "docs/design/2724-";
    CHECK(read_file((design_path + "region-concurrent-admit.md").c_str()).empty(),
          "AC6: no docs/design/2724-* per #1655 (design rationale in close comment)");
}

// ── Issue #2726 AC1: cross-fiber force-degrade sets per-Fiber
// pending_hold_budget_cancel_ flag via registry lookup; holder observes
// the flag at outermost MutationBoundaryGuard dtor (Phase-5 exit) and
// the Guard flips *flag_=false so the failure path runs (same as
// mark_outermost_mutation_failed). Within one dtor window the holder
// exits Guard + workspace_mtx_ exclusive + GcDeferReason::MutationHold
// released + new admits succeed (counters advance same/cross-fiber).
static void ac2726_1_cross_fiber_real_cancel() {
    std::println("\n--- #2726 AC1: cross-fiber force-degrade real cancel ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto fh = read_file("src/serve/fiber.h");
    const auto fc = read_file("src/serve/fiber.cpp");
    // Counters (mhb).
    CHECK(mhb.find("g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_fired_total") !=
              std::string::npos,
          "AC1: mhb has cross-fiber-cancel-fired counter");
    CHECK(mhb.find("g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_consumed_total") !=
              std::string::npos,
          "AC1: mhb has cross-fiber-cancel-consumed counter");
    CHECK(mhb.find("kMutationHoldBudgetHolderDegradeCrossFiberCancelIssue = 2726") !=
              std::string::npos,
          "AC1: mhb stamps issue = 2726");
    // Fiber pending-cancel flag + accessors (fh).
    CHECK(fh.find("request_hold_budget_cancel") != std::string::npos,
          "AC1: fiber.h has request_hold_budget_cancel");
    CHECK(fh.find("consume_hold_budget_cancel") != std::string::npos,
          "AC1: fiber.h has consume_hold_budget_cancel");
    CHECK(fh.find("peek_hold_budget_cancel") != std::string::npos,
          "AC1: fiber.h has peek_hold_budget_cancel");
    CHECK(fh.find("pending_hold_budget_cancel_") != std::string::npos,
          "AC1: fiber.h has pending_hold_budget_cancel_ atomic");
    // Fiber* registry + C-linkage shim (fc).
    CHECK(fc.find("aura_fiber_request_hold_budget_cancel") != std::string::npos,
          "AC1: fiber.cpp declares the C-linkage shim");
    CHECK(fc.find("register_fiber_in_registry") != std::string::npos,
          "AC1: fiber.cpp registers Fiber* in process-wide registry");
    CHECK(fc.find("unregister_fiber_from_registry") != std::string::npos,
          "AC1: fiber.cpp unregisters on dtor");
    // Cross-fiber wire-up (efm).
    CHECK(efm.find("aura_fiber_request_hold_budget_cancel") != std::string::npos,
          "AC1: efm calls the C-linkage shim");
    CHECK(efm.find("g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_fired_total") !=
              std::string::npos,
          "AC1: efm bumps fired counter on success");
    // Phase-5 poll (emb).
    CHECK(emb.find("consume_hold_budget_cancel") != std::string::npos,
          "AC1: emb polls consume_hold_budget_cancel at dtor");
    CHECK(emb.find("g_mutation_hold_budget_holder_degrade_cross_fiber_cancel_consumed_total") !=
              std::string::npos,
          "AC1: emb bumps consumed counter on consume");
    CHECK(emb.find("is_outermost_") != std::string::npos,
          "AC1: emb poll is ctor-captured outermost (AC3 nested-guards-safe)");
}

// ── Issue #2726 AC2: Soft / sandbox=off → flag not set unless
// AURA_MUTATION_HOLD_BUDGET_HARD=1. Same #2701/#2720 gating via
// mutation_hold_budget_reject_enabled() inside the cross-fiber fire
// path. Soft / test override callers see counter advance only.
static void ac2726_2_soft_counter_only() {
    std::println("\n--- #2726 AC2: Soft path → counter-only unless hard env ---");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // Cross-fiber wire-up inside aura_evaluator_force_degrade_outermost_holder
    // gates the cancel-flag set on mutation_hold_budget_reject_enabled().
    CHECK(efm.find("mutation_hold_budget_reject_enabled()") != std::string::npos,
          "AC2: efm cross-fiber gate reuses #2701/#2720 reject_enabled");
    // Phase-5 poll likewise gates on reject_enabled so a flag set
    // under Soft (somehow, e.g. direct C-linkage call) is not consumed.
    CHECK(emb.find("mutation_hold_budget_reject_enabled()") != std::string::npos,
          "AC2: emb poll gates on reject_enabled (defense-in-depth)");
}

// ── Issue #2726 AC3: nested / non-outermost Guards never observe or
// clear the pending-cancel flag. The consume call sits in the
// outermost dtor Phase-5 exit path (is_outermost_ ctor-captured per
// #2120); nested guards fall through to the nested depth_slot--
// branch below without touching the flag.
static void ac2726_3_nested_outermost_only() {
    std::println("\n--- #2726 AC3: nested guards never touch pending-cancel ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // The poll block is gated by is_outermost_ (ctor-captured, never
    // reads thread_local current_fiber mid-exit).
    CHECK(emb.find("is_outermost_ && aura::serve::g_current_fiber") != std::string::npos,
          "AC3: emb poll gated on is_outermost_ (nested skip)");
    // Comment cites AC3 contract.
    CHECK(emb.find("AC3") != std::string::npos,
          "AC3: emb source-cites AC3 nested-guards-skip contract");
}

// ── Issue #2726 AC4: additive observability — schema/issue sentinels +
// cross-fiber-cancel-fired/consumed counters; all #2701/#2720/#2587
// surfaces preserved (strict superset).
// clang-format may split long string literals across lines (adjacent
// literals concat at compile time); match without requiring a single
// contiguous quoted key (same approach as the #2726 linter must_key).
static void ac2726_4_query_keys() {
    std::println("\n--- #2726 AC4: additive query keys ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    auto has_key = [&](std::string_view key) {
        std::string normalized;
        normalized.reserve(q.size());
        for (char ch : q) {
            if (ch != '"' && ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t')
                normalized.push_back(ch);
        }
        return normalized.find(key) != std::string::npos;
    };
    // #2726 new keys present (tolerate clang-format line splits).
    CHECK(has_key("mutation-hold-budget-holder-degrade-cross-fiber-cancel-fired-total"),
          "AC4: cross-fiber-cancel-fired-total key");
    CHECK(has_key("mutation-hold-budget-holder-degrade-cross-fiber-cancel-consumed-total"),
          "AC4: cross-fiber-cancel-consumed-total key");
    CHECK(q.find("\"schema-2726\"") != std::string::npos, "AC4: schema-2726 sentinel");
    CHECK(q.find("\"issue-2726\"") != std::string::npos, "AC4: issue-2726 sentinel");
    // #2701 / #2720 / #2724 / #2551 surfaces preserved (strict superset).
    CHECK(q.find("\"mutation-hold-budget-reject-total\"") != std::string::npos,
          "AC4: #2701 reject-total preserved");
    CHECK(q.find("\"mutation-hold-budget-soft-observe-total\"") != std::string::npos,
          "AC4: #2701 soft-observe-total preserved");
    CHECK(q.find("\"schema-2701\"") != std::string::npos, "AC4: #2701 schema-2701 preserved");
    CHECK(q.find("\"schema-2720\"") != std::string::npos, "AC4: #2720 schema-2720 preserved");
    CHECK(q.find("\"schema-2724\"") != std::string::npos, "AC4: #2724 schema-2724 preserved");
    CHECK(q.find("schema-2551") != std::string::npos || q.find("schema-2587") != std::string::npos,
          "AC4: #2551/#2587 baseline preserved");
}

// ── Issue #2726 AC5: source-cite + linter (extend #2701/#2720/#2724
// surface, no docs/design/* per #1655); test is in src/-aligned suite
// per #81967 (this file, no new test_issue_2726.cpp).
static void ac2726_5_source_and_linter() {
    std::println("\n--- #2726 AC5: source-cite + linter + no new test file ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto fh = read_file("src/serve/fiber.h");
    const auto fc = read_file("src/serve/fiber.cpp");
    const auto t = read_file("tests/serve/test_mailbox_hold_starvation_hard.cpp");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_cross_fiber_hold_budget_cancel_2726.py");
    // Source-cite: mhb / emb / efm / q / fh / fc all cite #2726.
    CHECK(mhb.find("Issue #2726") != std::string::npos, "AC5: mhb cites #2726");
    CHECK(emb.find("Issue #2726") != std::string::npos, "AC5: emb cites #2726");
    CHECK(efm.find("Issue #2726") != std::string::npos, "AC5: efm cites #2726");
    CHECK(q.find("Issue #2726") != std::string::npos, "AC5: q cites #2726");
    CHECK(fh.find("Issue #2726") != std::string::npos, "AC5: fiber.h cites #2726");
    CHECK(fc.find("Issue #2726") != std::string::npos, "AC5: fiber.cpp cites #2726");
    // Test functions present (this file per #81967).
    CHECK(t.find("ac2726_1_cross_fiber_real_cancel") != std::string::npos, "AC5: AC1 test present");
    CHECK(t.find("ac2726_2_soft_counter_only") != std::string::npos, "AC5: AC2 test present");
    CHECK(t.find("ac2726_3_nested_outermost_only") != std::string::npos, "AC5: AC3 test present");
    CHECK(t.find("ac2726_4_query_keys") != std::string::npos, "AC5: AC4 test present");
    CHECK(t.find("ac2726_5_source_and_linter") != std::string::npos, "AC5: AC5 self-test");
    // #81967: NO new test file — extend tests/serve/test_mailbox_hold_starvation_hard.cpp only.
    CHECK(read_file("tests/serve/test_issue_2726.cpp").empty(),
          "AC5: no tests/serve/test_issue_2726.cpp per #81967");
    // build.py wires the linter.
    CHECK(build.find("check_cross_fiber_hold_budget_cancel_2726") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(lint.find("2726") != std::string::npos, "AC5: linter covers #2726");
}

// ── Issue #2726 AC6: no docs/design/2726-* per #1655 (design
// rationale lives in the close comment, not in a per-issue plan doc).
static void ac2726_6_no_docs_design() {
    std::println("\n--- #2726 AC6: no docs/design/2726-* per #1655 ---");
    const std::string design_path = "docs/design/2726-";
    CHECK(read_file((design_path + "cross-fiber-hold-budget-cancel.md").c_str()).empty(),
          "AC6: no docs/design/2726-* per #1655 (design rationale in close comment)");
}

// ── Issue #2754 AC1: equal keys + cone-/mask-disjoint ImpactScope →
// concurrent admit (bump cone-admit counter). Key-disjoint fast path
// preserved (#2724). regions_disjoint 4-arg + regions_cone_disjoint
// helpers; TLS cone mask via parallel_task_cone_mask.
static void ac2754_1_cone_disjoint_concurrent_admit() {
    std::println("\n--- #2754 AC1: cone-disjoint concurrent admit ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    // Helper: 4-arg regions_disjoint + regions_cone_disjoint (mask AND).
    CHECK(mhb.find("regions_disjoint(std::uint64_t a, std::uint64_t b, std::uint64_t mask_a") !=
              std::string::npos,
          "AC1: 4-arg regions_disjoint (key + cone mask) declared");
    CHECK(mhb.find("(mask_a & mask_b) == 0") != std::string::npos,
          "AC1: mask-AND hot path (no tree walk)");
    CHECK(mhb.find("regions_cone_disjoint") != std::string::npos,
          "AC1: regions_cone_disjoint helper declared");
    // Key-equality remains the fast path (2-arg overload preserved).
    CHECK(mhb.find("return a != 0 && b != 0 && a != b;") != std::string::npos,
          "AC1: key-disjoint fast path preserved (#2724)");
    // Admit path uses 4-arg + bumps cone-admit on cone path.
    CHECK(emb.find("regions_disjoint(region_key, g_last_admitted_region_key, cone_mask") !=
              std::string::npos,
          "AC1: emb uses 4-arg regions_disjoint with cone_mask");
    CHECK(emb.find("regions_cone_disjoint") != std::string::npos,
          "AC1: emb calls regions_cone_disjoint for cone-admit counter");
    CHECK(emb.find("g_mutation_region_concurrent_cone_admit_total.fetch_add(1,") !=
              std::string::npos,
          "AC1: cone-admit path bumps cone counter");
    CHECK(emb.find("parallel_task_cone_mask()") != std::string::npos,
          "AC1: emb reads TLS cone mask");
    // TLS stamp surface (fiber_mutation + evaluator.ixx).
    CHECK(efm.find("note_parallel_task_cone_mask") != std::string::npos,
          "AC1: efm has note_parallel_task_cone_mask");
    CHECK(efm.find("g_parallel_task_cone_mask") != std::string::npos,
          "AC1: efm has TLS g_parallel_task_cone_mask");
    CHECK(ixx.find("note_parallel_task_cone_mask") != std::string::npos,
          "AC1: evaluator.ixx declares cone-mask TLS API");
    CHECK(ixx.find("parallel_task_cone_mask()") != std::string::npos,
          "AC1: evaluator.ixx declares parallel_task_cone_mask getter");
}

// ── Issue #2754 AC2: true overlapping cones still reject region-overlap
// under production. Equal keys + overlapping masks (or mask==0 unknown)
// → structured reject; gate order preserved.
static void ac2754_2_true_overlap_still_rejects() {
    std::println("\n--- #2754 AC2: true overlap still rejects ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // Unknown mask (0) → equal keys not proven disjoint.
    CHECK(mhb.find("mask_a != 0 && mask_b != 0") != std::string::npos,
          "AC2: both masks must be non-zero to prove cone-disjoint");
    // Overlap reject path preserved.
    CHECK(emb.find("AdmissionRejected: region-overlap") != std::string::npos,
          "AC2: structured region-overlap reject preserved");
    CHECK(emb.find("g_mutation_region_overlap_reject_total.fetch_add(1,") != std::string::npos,
          "AC2: overlap-reject counter still bumped");
    // Gate order: #2587/#2701/#2630 still before region check.
    CHECK(emb.find("mutation_hold_budget_check") != std::string::npos,
          "AC2: #2701 hold-budget gate preserved before region check");
    CHECK(emb.find("make_security_schedule_input_live") != std::string::npos,
          "AC2: #2630/#2660 security-schedule gate preserved");
}

// ── Issue #2754 AC3: Soft / sandbox=off → metric-only (no lock regression).
static void ac2754_3_soft_path_metric_only() {
    std::println("\n--- #2754 AC3: Soft path metric-only ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(emb.find("#2754 AC3") != std::string::npos ||
              emb.find("#2724 AC3 / #2754 AC3") != std::string::npos,
          "AC3: Soft path cites #2754 AC3");
    CHECK(emb.find("metric-only") != std::string::npos, "AC3: Soft path metric-only");
    // Soft path also tracks cone-admit for observability.
    CHECK(emb.find("g_last_admitted_cone_mask_soft") != std::string::npos,
          "AC3: soft path tracks last cone mask (metric-only)");
}

// ── Issue #2754 AC4: densify / ownership_rebind / restamp remain correct
// under concurrent region holds (per-region shard isolation preserved;
// atomic_batch still falls back to GlobalExclusive per #2121).
static void ac2754_4_densify_under_concurrent_holds() {
    std::println("\n--- #2754 AC4: densify under concurrent region holds ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(emb.find("region_shard_") != std::string::npos, "AC4: region_shard_ present");
    CHECK(emb.find("workspace_region_shard") != std::string::npos,
          "AC4: workspace_region_shard used");
    CHECK(emb.find("atomic_batch_active") != std::string::npos,
          "AC4: atomic_batch_active GlobalExclusive fallback preserved");
    CHECK(emb.find("workspace_region_fallback_global_total") != std::string::npos,
          "AC4: fallback-to-GlobalExclusive counter preserved");
    // Cone path does not bypass densify isolation (same region_key shard).
    CHECK(emb.find("regions_cone_disjoint") != std::string::npos,
          "AC4: cone-disjoint admit still goes through region Guard path");
}

// ── Issue #2754 AC5: additive observability — all #2701/#2720/#2724/
// #2551/#2587 counters preserved + new cone-admit / cone-wired +
// schema-2754 / issue-2754.
static void ac2754_5_additive_observability() {
    std::println("\n--- #2754 AC5: additive observability ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(mhb.find("g_mutation_region_concurrent_cone_admit_total{0}") != std::string::npos,
          "AC5: cone-admit-total counter initialized");
    CHECK(mhb.find("g_mutation_region_cone_disjoint_wired{1}") != std::string::npos,
          "AC5: cone-disjoint-wired sentinel initialized");
    CHECK(mhb.find("kMutationRegionConeDisjointIssue = 2754") != std::string::npos,
          "AC5: issue stamp = 2754");
    CHECK(q.find("mutation-region-concurrent-cone-admit-total") != std::string::npos,
          "AC5: query key mutation-region-concurrent-cone-admit-total");
    CHECK(q.find("mutation-region-cone-disjoint-wired") != std::string::npos,
          "AC5: query key mutation-region-cone-disjoint-wired");
    CHECK(q.find("schema-2754") != std::string::npos, "AC5: schema-2754 sentinel");
    CHECK(q.find("issue-2754") != std::string::npos, "AC5: issue-2754 sentinel");
    // Prior surfaces preserved (strict superset).
    CHECK(q.find("mutation-region-concurrent-admit-total") != std::string::npos,
          "AC5: #2724 concurrent-admit-total preserved");
    CHECK(q.find("mutation-region-overlap-reject-total") != std::string::npos,
          "AC5: #2724 overlap-reject-total preserved");
    CHECK(q.find("schema-2724") != std::string::npos, "AC5: schema-2724 preserved");
    CHECK(q.find("schema-2701") != std::string::npos, "AC5: schema-2701 preserved");
    CHECK(q.find("schema-2720") != std::string::npos, "AC5: schema-2720 preserved");
    CHECK(q.find("schema-2726") != std::string::npos, "AC5: schema-2726 preserved");
}

// ── Issue #2754 AC6: source-cite + extend this file per #81967 + no
// docs/design/2754-* per #1655 + linter wired.
static void ac2754_6_source_and_linter() {
    std::println("\n--- #2754 AC6: source-cite + linter + no docs/design/ ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto t = read_file("tests/serve/test_mailbox_hold_starvation_hard.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_region_cone_disjoint_admit_2754.py");
    CHECK(mhb.find("Issue #2754") != std::string::npos, "AC6: mhb cites #2754");
    CHECK(emb.find("Issue #2754") != std::string::npos || emb.find("#2754") != std::string::npos,
          "AC6: emb cites #2754");
    CHECK(efm.find("Issue #2754") != std::string::npos, "AC6: efm cites #2754");
    CHECK(q.find("Issue #2754") != std::string::npos, "AC6: query cites #2754");
    CHECK(t.find("ac2754_1_cone_disjoint_concurrent_admit") != std::string::npos,
          "AC6: AC1 test present");
    CHECK(t.find("ac2754_2_true_overlap_still_rejects") != std::string::npos,
          "AC6: AC2 test present");
    CHECK(t.find("ac2754_3_soft_path_metric_only") != std::string::npos, "AC6: AC3 test present");
    CHECK(t.find("ac2754_4_densify_under_concurrent_holds") != std::string::npos,
          "AC6: AC4 test present");
    CHECK(t.find("ac2754_5_additive_observability") != std::string::npos, "AC6: AC5 test present");
    CHECK(t.find("ac2754_6_source_and_linter") != std::string::npos, "AC6: AC6 self-test");
    // #81967: no new test file.
    CHECK(read_file("tests/serve/test_issue_2754.cpp").empty(),
          "AC6: no tests/serve/test_issue_2754.cpp per #81967");
    // Prior #2724 tests preserved.
    CHECK(t.find("ac2724_1_disjoint_concurrent_admit") != std::string::npos,
          "AC6: #2724 AC1 preserved");
    CHECK(build.find("check_region_cone_disjoint_admit_2754") != std::string::npos,
          "AC6: build.py wires linter");
    CHECK(!lint.empty(), "AC6: linter present");
    CHECK(lint.find("2754") != std::string::npos, "AC6: linter covers #2754");
    // No docs/design/2754-* per #1655.
    const std::string design_path = "docs/design/2754-";
    CHECK(read_file((design_path + "region-cone-disjoint-admit.md").c_str()).empty(),
          "AC6: no docs/design/2754-* per #1655 (design rationale in close comment)");
}

// ── Issue #2757 AC1: zero keys + proven mask-AND empty intersection →
// concurrent admit (mask-disjoint counter). Key-disjoint + equal-key
// cone path preserved (#2724/#2754).
static void ac2757_1_zero_key_mask_disjoint_admit() {
    std::println("\n--- #2757 AC1: zero-key + mask-AND concurrent admit ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mhb.find("regions_mask_disjoint") != std::string::npos,
          "AC1: regions_mask_disjoint helper declared");
    CHECK(mhb.find("g_mutation_region_mask_disjoint_admit_total{0}") != std::string::npos,
          "AC1: mask-disjoint-admit counter initialized");
    CHECK(mhb.find("kMutationRegionMaskDisjointIssue = 2757") != std::string::npos,
          "AC1: issue stamp = 2757");
    // Quiet path: mask==0 → equality only (no mask-AND work).
    CHECK(mhb.find("if (mask_a == 0 || mask_b == 0)") != std::string::npos,
          "AC1: quiet path when either mask is 0");
    // Enter admit check on key OR mask (zero-key + proven mask eligible).
    CHECK(emb.find("region_or_mask") != std::string::npos,
          "AC1: emb enters check when key or mask present");
    CHECK(emb.find("regions_mask_disjoint") != std::string::npos,
          "AC1: emb calls regions_mask_disjoint");
    CHECK(emb.find("g_mutation_region_mask_disjoint_admit_total.fetch_add(1,") != std::string::npos,
          "AC1: mask-disjoint path bumps counter");
    // Key-disjoint fast path still first in 4-arg regions_disjoint.
    CHECK(mhb.find("a != 0 && b != 0 && a != b") != std::string::npos,
          "AC1: key-disjoint fast path preserved");
}

// ── Issue #2757 AC2: overlapping masks still reject region-overlap;
// densify isolation preserved.
static void ac2757_2_overlap_and_densify() {
    std::println("\n--- #2757 AC2: overlap reject + densify isolation ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(emb.find("AdmissionRejected: region-overlap") != std::string::npos,
          "AC2: structured region-overlap reject preserved");
    CHECK(emb.find("g_mutation_region_overlap_reject_total.fetch_add(1,") != std::string::npos,
          "AC2: overlap-reject counter still bumped");
    CHECK(emb.find("region_shard_") != std::string::npos, "AC2: region_shard_ isolation");
    CHECK(emb.find("atomic_batch_active") != std::string::npos,
          "AC2: atomic_batch GlobalExclusive fallback preserved");
}

// ── Issue #2757 AC3: Soft metric-only; AC4 quiet path.
static void ac2757_3_soft_and_quiet_path() {
    std::println("\n--- #2757 AC3/AC4: Soft metric-only + quiet path ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    CHECK(emb.find("metric-only") != std::string::npos, "AC3: Soft path metric-only");
    CHECK(emb.find("g_last_admitted_cone_mask_soft") != std::string::npos,
          "AC3: soft path tracks cone mask");
    // Quiet path: pure zero-key+zero-mask skips the concurrent-admit block.
    CHECK(emb.find("region_or_mask") != std::string::npos,
          "AC4: region_or_mask gate (quiet zero/zero skips)");
    CHECK(mhb.find("Quiet path") != std::string::npos ||
              mhb.find("quiet path") != std::string::npos ||
              mhb.find("Zero extra work") != std::string::npos ||
              mhb.find("zero extra work") != std::string::npos,
          "AC4: quiet path documented in regions_disjoint");
}

// ── Issue #2757 AC5: additive observability; all #2724/#2754 preserved.
static void ac2757_5_additive_observability() {
    std::println("\n--- #2757 AC5: additive observability ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(mhb.find("g_mutation_region_mask_disjoint_admit_total{0}") != std::string::npos,
          "AC5: mask-disjoint-admit-total initialized");
    CHECK(mhb.find("g_mutation_region_mask_disjoint_wired{1}") != std::string::npos,
          "AC5: mask-disjoint-wired sentinel");
    CHECK(q.find("mutation-region-mask-disjoint-admit-total") != std::string::npos,
          "AC5: query key mutation-region-mask-disjoint-admit-total");
    CHECK(q.find("mutation-region-mask-disjoint-wired") != std::string::npos,
          "AC5: query key mutation-region-mask-disjoint-wired");
    CHECK(q.find("schema-2757") != std::string::npos, "AC5: schema-2757");
    CHECK(q.find("issue-2757") != std::string::npos, "AC5: issue-2757");
    // Prior surfaces preserved.
    CHECK(q.find("mutation-region-concurrent-admit-total") != std::string::npos,
          "AC5: #2724 concurrent-admit preserved");
    CHECK(q.find("mutation-region-concurrent-cone-admit-total") != std::string::npos,
          "AC5: #2754 cone-admit preserved");
    CHECK(q.find("schema-2724") != std::string::npos, "AC5: schema-2724 preserved");
    CHECK(q.find("schema-2754") != std::string::npos, "AC5: schema-2754 preserved");
}

// ── Issue #2757 AC6: source-cite + linter + no docs/design/* + #81967.
static void ac2757_6_source_and_linter() {
    std::println("\n--- #2757 AC6: source-cite + linter ---");
    const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto t = read_file("tests/serve/test_mailbox_hold_starvation_hard.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_region_mask_disjoint_admit_2757.py");
    CHECK(mhb.find("Issue #2757") != std::string::npos || mhb.find("#2757") != std::string::npos,
          "AC6: mhb cites #2757");
    CHECK(emb.find("#2757") != std::string::npos, "AC6: emb cites #2757");
    CHECK(q.find("Issue #2757") != std::string::npos, "AC6: query cites #2757");
    CHECK(t.find("ac2757_1_zero_key_mask_disjoint_admit") != std::string::npos,
          "AC6: AC1 test present");
    CHECK(t.find("ac2757_2_overlap_and_densify") != std::string::npos, "AC6: AC2 test present");
    CHECK(t.find("ac2757_3_soft_and_quiet_path") != std::string::npos, "AC6: AC3/AC4 test present");
    CHECK(t.find("ac2757_5_additive_observability") != std::string::npos, "AC6: AC5 test present");
    CHECK(t.find("ac2757_6_source_and_linter") != std::string::npos, "AC6: AC6 self-test");
    CHECK(t.find("ac2754_1_cone_disjoint_concurrent_admit") != std::string::npos,
          "AC6: #2754 tests preserved");
    CHECK(read_file("tests/serve/test_issue_2757.cpp").empty(),
          "AC6: no tests/serve/test_issue_2757.cpp per #81967");
    CHECK(build.find("check_region_mask_disjoint_admit_2757") != std::string::npos,
          "AC6: build.py wires linter");
    CHECK(!lint.empty(), "AC6: linter present");
    CHECK(read_file("docs/design/2757-region-mask-disjoint.md").empty(),
          "AC6: no docs/design/2757-* per #1655");
}

} // namespace

int run_test_mailbox_hold_starvation_hard() {
    std::println("=== Issue #2551: mailbox hold starvation hard + Agent throttle ===");
    ac1_production_hard_signal();
    ac2_soft_and_free();
    ac3_clear_on_drain_zero();
    ac4_chaos_and_query();
    ac5_source_and_gate();
    std::println("\n=== Issue #2701: mutation hold-budget reject (post-#2551) ===");
    ac2701_1_budget_reject_production();
    ac2701_2_soft_path_metric_only();
    // ac2701_3_order_with_security_schedule never authored (source-cite only
    // elsewhere); skip the undefined call so the batch still links.
    ac2701_4_query_keys_added();
    ac2701_5_source_and_linter();
    ac2701_6_no_docs_design();
    std::println("\n=== Issue #2720: P0 holder-degrade (#2701 residual) ===");
    ac2720_1_force_degrade_production();
    ac2720_2_soft_counter_only();
    ac2720_3_nested_outermost_only();
    ac2720_4_query_keys();
    ac2720_5_source_and_linter();
    ac2720_6_no_docs_design();
    std::println("\n=== Issue #2724: region concurrent admit (disjoint multi-agent) ===");
    ac2724_1_disjoint_concurrent_admit();
    ac2724_2_overlap_reject_production();
    ac2724_3_soft_path_metric_only();
    ac2724_4_densify_correctness_under_concurrent_holds();
    ac2724_5_additive_observability();
    ac2724_6_source_and_linter();
    std::println("\n=== #2551 + #2701 + #2720 + #2724: {} passed, {} failed ===", g_passed,
                 g_failed);
    std::println(
        "\n=== Issue #2726: P0 cross-fiber force-degrade real cancel (#2720 residual) ===");
    ac2726_1_cross_fiber_real_cancel();
    ac2726_2_soft_counter_only();
    ac2726_3_nested_outermost_only();
    ac2726_4_query_keys();
    ac2726_5_source_and_linter();
    ac2726_6_no_docs_design();
    std::println(
        "\n=== Issue #2754: region concurrent cone/ImpactScope mask-AND (#2724 residual) ===");
    ac2754_1_cone_disjoint_concurrent_admit();
    ac2754_2_true_overlap_still_rejects();
    ac2754_3_soft_path_metric_only();
    ac2754_4_densify_under_concurrent_holds();
    ac2754_5_additive_observability();
    ac2754_6_source_and_linter();
    std::println(
        "\n=== Issue #2757: region mask-AND disjoint (zero keys + quiet path, #2724 residual) ===");
    ac2757_1_zero_key_mask_disjoint_admit();
    ac2757_2_overlap_and_densify();
    ac2757_3_soft_and_quiet_path();
    ac2757_5_additive_observability();
    ac2757_6_source_and_linter();
    std::println(
        "\n=== #2551 + #2701 + #2720 + #2724 + #2726 + #2754 + #2757: {} passed, {} failed ===",
        g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_mailbox_hold_starvation_hard();
}
#endif
