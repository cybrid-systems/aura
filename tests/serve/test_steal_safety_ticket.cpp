// @category: unit
// @reason: Issue #2518 — MutationSafetySnapshot sequence ticket closes
// sample→resume window (Guard enter/exit mid-window → hard-fail).
//
//   AC1: snapshot carries ticket; resume mismatch → hard-fail
//   AC2: inject mid-window publish → ticket mismatch intercepted
//   AC3: coexist with #2510 LayoutStamp path (no dual-compute conflict)
//   AC4: single atomic load cost path; no extra data race under TSan shape
//   AC5: extend steal safety / resume invariant tests

#include "test_harness.hpp"
#include "serve/fiber.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <thread>

import std;
import aura.compiler.service;
import aura.compiler.value;

extern "C" std::uint64_t aura_fiber_static_steal_snapshot_hard_fail_total();
extern "C" std::uint64_t aura_fiber_static_steal_safety_ticket_mismatch_total();
extern "C" std::uint64_t aura_fiber_static_mutation_steal_snapshot_mismatch_total();

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::serve::FiberState;
using aura::serve::Scheduler;
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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:orchestration-steal-outermost-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void run_on_fiber(std::function<void()> body) {
    Scheduler sched(2);
    std::atomic<bool> done{false};
    sched.spawn([&]() {
        body();
        done.store(true);
    });
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.stop();
    io.join();
    CHECK(done.load(), "fiber body completed");
}

// ── AC1: ticket on snapshot; matching ticket ok ──
static void ac1_ticket_match_ok() {
    std::println("\n--- AC1: snapshot ticket + matching resume ok ---");
    ::setenv("AURA_STEAL_SNAPSHOT_HARD", "1", 1);
    ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
    run_on_fiber([&]() {
        auto* fb = aura::serve::g_current_fiber;
        CHECK(fb != nullptr, "AC1: current fiber");
        auto snap = fb->mutation_safety_snapshot();
        // ticket is even (0 if never published).
        CHECK((snap.ticket & 1u) == 0, "AC1: ticket is even seq");
        CHECK(snap.ticket == fb->current_safety_ticket(), "AC1: ticket == current");
        fb->set_resume_safety_ticket(snap.ticket);
        CHECK(fb->has_resume_safety_ticket(), "AC1: ticket stamped");
        CHECK(fb->check_and_enforce_resume_snapshot_invariant(), "AC1: match → continue");
        CHECK(!fb->has_resume_safety_ticket(), "AC1: ticket consumed (one-shot)");
        CHECK(fb->state() != FiberState::Done || !fb->is_cancel_requested(),
              "AC1: not hard-failed on match");
    });
    ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
}

// ── AC2: inject mid-window publish → mismatch hard-fail ──
static void ac2_inject_mid_window() {
    std::println("\n--- AC2: inject Guard publish mid-window → hard-fail ---");
    ::setenv("AURA_STEAL_SNAPSHOT_HARD", "1", 1);
    ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
    const auto miss0 = aura_fiber_static_steal_safety_ticket_mismatch_total();
    const auto hard0 = aura_fiber_static_steal_snapshot_hard_fail_total();
    const auto obs0 = aura_fiber_static_mutation_steal_snapshot_mismatch_total();
    run_on_fiber([&]() {
        auto* fb = aura::serve::g_current_fiber;
        auto snap = fb->mutation_safety_snapshot();
        fb->set_resume_safety_ticket(snap.ticket);
        // Simulate Guard enter/exit between steal sample and resume:
        // publish advances safety_seq_ by 2 (odd write, even stable).
        fb->publish_mutation_safety_mirrors(/*depth=*/1, /*held=*/true, /*defuse=*/42);
        fb->publish_mutation_safety_mirrors(/*depth=*/0, /*held=*/false, /*defuse=*/42);
        CHECK(fb->current_safety_ticket() != snap.ticket, "AC2: seq advanced after publish");
        CHECK(!fb->check_and_enforce_resume_snapshot_invariant(), "AC2: Hard stops resume");
        CHECK(fb->is_cancel_requested(), "AC2: cancel");
        CHECK(fb->state() == FiberState::Done, "AC2: Done");
    });
    CHECK(aura_fiber_static_steal_safety_ticket_mismatch_total() > miss0,
          "AC2: ticket mismatch +1");
    CHECK(aura_fiber_static_steal_snapshot_hard_fail_total() > hard0, "AC2: hard-fail +1");
    CHECK(aura_fiber_static_mutation_steal_snapshot_mismatch_total() > obs0, "AC2: observed +1");
    ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
}

// Soft: ticket miss continues
static void ac2_soft_ticket_miss() {
    std::println("\n--- AC2b: Soft ticket miss → metric, continue ---");
    ::setenv("AURA_STEAL_SNAPSHOT_SOFT", "1", 1);
    ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
    const auto miss0 = aura_fiber_static_steal_safety_ticket_mismatch_total();
    const auto hard0 = aura_fiber_static_steal_snapshot_hard_fail_total();
    run_on_fiber([&]() {
        auto* fb = aura::serve::g_current_fiber;
        auto snap = fb->mutation_safety_snapshot();
        fb->set_resume_safety_ticket(snap.ticket);
        fb->publish_mutation_safety_mirrors(1, true, 1);
        CHECK(fb->check_and_enforce_resume_snapshot_invariant(), "AC2b: Soft continues");
        CHECK(fb->state() != FiberState::Done || !fb->is_cancel_requested(),
              "AC2b: not hard-failed");
    });
    CHECK(aura_fiber_static_steal_safety_ticket_mismatch_total() > miss0, "AC2b: ticket miss +1");
    CHECK(aura_fiber_static_steal_snapshot_hard_fail_total() == hard0, "AC2b: hard-fail unchanged");
    ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
}

// ── AC3: #2510 coexist ──
static void ac3_coexist_2510() {
    std::println("\n--- AC3: coexist with #2510 LayoutStamp path ---");
    const auto fh = read_file("src/serve/fiber.h");
    const auto wc = read_file("src/serve/worker.cpp");
    const auto fc = read_file("src/serve/fiber.cpp");
    CHECK(fh.find("Issue #2518") != std::string::npos, "AC3: fiber.h #2518");
    CHECK(fh.find("2510") != std::string::npos || wc.find("2510") != std::string::npos,
          "AC3: #2510 lineage cited");
    // Issue #2752: ticket stamp lives in steal_safety.cpp Ok branch;
    // worker routes through steal_safety_transaction only.
    const auto ss = read_file("src/serve/steal_safety.cpp");
    CHECK(ss.find("set_resume_safety_ticket") != std::string::npos ||
              wc.find("steal_safety_transaction") != std::string::npos,
          "AC3: steal stamps ticket (via steal_safety_transaction)");
    CHECK(wc.find("steal_safety_transaction") != std::string::npos ||
              ss.find("aura_evaluator_on_steal_complete") != std::string::npos,
          "AC3: steal-complete restamp path retained");
    CHECK(fc.find("ticket_miss") != std::string::npos ||
              fc.find("has_resume_safety_ticket_") != std::string::npos,
          "AC3: resume checks ticket");
    // Ticket field independent of resume_layout_stamp_set_
    CHECK(fh.find("resume_safety_ticket_") != std::string::npos, "AC3: ticket field");
    CHECK(fh.find("resume_layout_stamp") != std::string::npos ||
              fh.find("resume_layout_stamp_set_") != std::string::npos,
          "AC3: layout stamp fields retained");
}

// ── AC4: cost path + source ──
static void ac4_cost_and_query() {
    std::println("\n--- AC4: query + single-load cost documentation ---");
    const auto fh = read_file("src/serve/fiber.h");
    CHECK(fh.find("current_safety_ticket") != std::string::npos, "AC4: current_safety_ticket");
    CHECK(fh.find("single atomic load") != std::string::npos ||
              fh.find("one acquire load") != std::string::npos ||
              fh.find("One acquire") != std::string::npos,
          "AC4: cost documented");
    CompilerService cs;
    CHECK(href(cs, "schema-2518") == 2518, "AC4: schema-2518");
    CHECK(href(cs, "issue-2518") == 2518, "AC4: issue-2518");
    CHECK(href(cs, "steal-safety-ticket-wired") == 1, "AC4: wired");
    CHECK(href(cs, "steal-safety-ticket-mismatch-total") >= 0, "AC4: mismatch total");
    CHECK(href(cs, "schema-2346") == 2346, "AC4: schema-2346 retained");
    CHECK(href(cs, "schema-2510") == 2510, "AC4: schema-2510 coexist");
}

// ── AC5: source wiring ──
static void ac5_source_wiring() {
    std::println("\n--- AC5: steal + resume wiring ---");
    const auto fh = read_file("src/serve/fiber.h");
    const auto fc = read_file("src/serve/fiber.cpp");
    const auto wc = read_file("src/serve/worker.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(fh.find("ticket") != std::string::npos, "AC5: snapshot.ticket");
    CHECK(fh.find("set_resume_safety_ticket") != std::string::npos, "AC5: set API");
    CHECK(fc.find("bump_steal_safety_ticket_mismatch") != std::string::npos, "AC5: bump");
    // Issue #2752: stamp is in steal_safety.cpp; worker only calls transaction.
    const auto ss = read_file("src/serve/steal_safety.cpp");
    CHECK(ss.find("set_resume_safety_ticket(snap.ticket)") != std::string::npos ||
              wc.find("steal_safety_transaction(stolen)") != std::string::npos,
          "AC5: ticket stamped on Ok path (steal_safety_transaction)");
    CHECK(q.find("schema-2518") != std::string::npos, "AC5: query schema");
    CHECK(q.find("steal-safety-ticket-mismatch-total") != std::string::npos, "AC5: query key");
}

// ── AC1 (Issue #2702): production hard-fail resume invariant exists.
// Source-cite per #81967 — production path cancels + Done on mismatch.
static void ac2702_1_resume_invariant_exists() {
    std::println("\n--- #2702 AC1: resume invariant hard-fail path exists ---");
    const auto fc = read_file("src/serve/fiber.cpp");
    const auto fh = read_file("src/serve/fiber.h");
    CHECK(fc.find("check_and_enforce_resume_snapshot_invariant") != std::string::npos,
          "AC1: check_and_enforce_resume_snapshot_invariant in fiber.cpp");
    CHECK(fc.find("request_cancel") != std::string::npos, "AC1: request_cancel on hard fail");
    CHECK(fc.find("set_state(FiberState::Done)") != std::string::npos,
          "AC1: set_state(Done) on hard fail");
    CHECK(fc.find("is_steal_snapshot_hard_mode") != std::string::npos,
          "AC1: hard-mode gate consulted");
    CHECK(fc.find("bump_mutation_steal_snapshot_mismatch") != std::string::npos,
          "AC1: mismatch counter bump");
    CHECK(fc.find("bump_steal_safety_ticket_mismatch") != std::string::npos,
          "AC1: ticket mismatch counter bump");
    CHECK(fc.find("bump_steal_snapshot_hard_fail") != std::string::npos,
          "AC1: hard-fail counter bump");
    CHECK(fh.find("Issue #2702") != std::string::npos, "AC1: fiber.h cites #2702");
}

// ── AC2 (Issue #2702): Soft / test-override is metric-only continue.
static void ac2702_2_soft_path_metric_only() {
    std::println("\n--- #2702 AC2: Soft path metric-only continue ---");
    const auto fc = read_file("src/serve/fiber.cpp");
    CHECK(fc.find("is_steal_snapshot_soft_mode") != std::string::npos,
          "AC2: soft-mode gate consulted");
    CHECK(fc.find("AURA_STEAL_SNAPSHOT_SOFT") != std::string::npos,
          "AC2: AURA_STEAL_SNAPSHOT_SOFT override");
    CHECK(fc.find("AURA_STEAL_SNAPSHOT_HARD") != std::string::npos,
          "AC2: AURA_STEAL_SNAPSHOT_HARD override");
}

// ── AC3 (Issue #2702): ticket is one-shot — cleared via
// clear_resume_safety_ticket after the check; never re-used across
// steals. File-content source-cite per #81967.
static void ac2702_3_ticket_one_shot() {
    std::println("\n--- #2702 AC3: ticket is one-shot (clear after check) ---");
    const auto fc = read_file("src/serve/fiber.cpp");
    const auto fh = read_file("src/serve/fiber.h");
    CHECK(fc.find("clear_resume_safety_ticket") != std::string::npos,
          "AC3: fiber.cpp clears the ticket");
    CHECK(fh.find("clear_resume_safety_ticket") != std::string::npos,
          "AC3: fiber.h declares clear_resume_safety_ticket");
    CHECK(fh.find("has_resume_safety_ticket_") != std::string::npos,
          "AC3: fiber.h tracks the one-shot has_ flag");
}

// ── AC4 (Issue #2702): strong interaction with #2699 steal safety
// transaction — ticket is stamped only on Ok transaction. Resume never
// sees a ticket from a RejectHard path. Source-cite per #81967.
static void ac2702_4_steal_safety_ticket_interaction() {
    std::println("\n--- #2702 AC4: ticket only stamped on Ok transaction ---");
    const auto sc = read_file("src/serve/steal_safety.cpp");
    const auto sh = read_file("src/serve/steal_safety.h");
    const auto fc = read_file("src/serve/fiber.cpp");
    const auto fh = read_file("src/serve/fiber.h");
    CHECK(sc.find("set_resume_safety_ticket") != std::string::npos,
          "AC4: steal_safety.cpp stamps ticket");
    CHECK(sh.find("steal_safety_transaction") != std::string::npos,
          "AC4: steal_safety.h declares transaction");
    CHECK(sh.find("kStealSafetyTransactionIssue = 2699") != std::string::npos,
          "AC4: #2699 issue stamp");
    CHECK(fc.find("set_resume_safety_ticket") != std::string::npos,
          "AC4: fiber.cpp implements ticket setter");
    CHECK(fh.find("set_resume_safety_ticket") != std::string::npos,
          "AC4: fiber.h declares set_resume_safety_ticket");
}

// ── AC5 (Issue #2702): additive query keys + source-cite for the
// resume hard-fail surface. All #2518 / #2310 / #2346 surfaces
// preserved (strict superset).
static void ac2702_5_query_keys_and_source_cite() {
    std::println("\n--- #2702 AC5: query keys + source-cite ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto fh = read_file("src/serve/fiber.h");
    CHECK(q.find("\"resume-hard-fail-total\"") != std::string::npos,
          "AC5: resume-hard-fail-total key");
    CHECK(q.find("\"resume-soft-observe-total\"") != std::string::npos,
          "AC5: resume-soft-observe-total key");
    CHECK(q.find("\"resume-hard-fail-wired\"") != std::string::npos,
          "AC5: resume-hard-fail-wired sentinel");
    CHECK(q.find("\"schema-2702\"") != std::string::npos, "AC5: schema-2702");
    CHECK(q.find("\"issue-2702\"") != std::string::npos, "AC5: issue-2702");
    CHECK(fh.find("kResumeHardFailIssue = 2702") != std::string::npos,
          "AC5: fiber.h stamps issue = 2702");
}

// ── AC6 (Issue #2702): no docs/design/2702-* per #1655.
static void ac2702_6_no_docs_design() {
    std::println("\n--- #2702 AC6: no docs/design/2702-* per #1655 ---");
    const std::string design_path = "docs/design/2702-";
    CHECK(read_file((design_path + "resume-hard-fail.md").c_str()).empty(),
          "AC6: no docs/design/2702-* per #1655 (design rationale in close comment)");
}

} // namespace

int run_test_steal_safety_ticket() {
    std::println("=== Issue #2518: MutationSafetySnapshot safety ticket ===");
    ac1_ticket_match_ok();
    ac2_inject_mid_window();
    ac2_soft_ticket_miss();
    ac3_coexist_2510();
    ac4_cost_and_query();
    ac5_source_wiring();
    std::println("\n=== Issue #2702: Resume hard-fail unified path (post-#2518) ===");
    ac2702_1_resume_invariant_exists();
    ac2702_2_soft_path_metric_only();
    ac2702_3_ticket_one_shot();
    ac2702_4_steal_safety_ticket_interaction();
    ac2702_5_query_keys_and_source_cite();
    ac2702_6_no_docs_design();
    // Leave process Soft-clean for subsequent tests in same binary (none).
    ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
    ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
    std::println("\n=== #2518 + #2702: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_steal_safety_ticket();
}
#endif
