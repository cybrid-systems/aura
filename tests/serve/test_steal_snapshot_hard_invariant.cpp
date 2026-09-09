// @category: unit
// @reason: Issue #2346 — resume MutationSafetySnapshot hard-invariant
// (fail-closed canary). Soft: mismatch metric only. Hard: mark-failed.

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

extern "C" void aura_evaluator_test_push_mutation_checkpoint();
extern "C" void aura_evaluator_test_pop_mutation_checkpoint();
extern "C" std::uint64_t aura_fiber_static_mutation_steal_snapshot_mismatch_total();
extern "C" std::uint64_t aura_fiber_static_steal_snapshot_hard_fail_total();

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

// ── Issue #3621: enumerative machine proof that sample→enqueue cannot read
// thief TLS (I3 residual of #2929/#3072). Linter-style source-cite ACs over
// steal_safety_transaction + mutation_safety_snapshot; reuses the
// #2721/#2901/#2929 residual hard-AND names. No new invariant enum —
// identity is the gap (#3617 closed it code-side; this pins it).
static void ac3621_1_depth_victim_storage_identity() {
    std::println("\n--- #3621 AC1/AC6: snapshot depth resolves via victim storage, never thief "
                 "TLS ---");
    const auto fh = read_file("src/serve/fiber.h");
    const auto ss = read_file("src/serve/steal_safety.cpp");
    // Victim storage helper is the only depth source on the steal path.
    CHECK(fh.find("aura_evaluator_mutation_stack_depth_from_ptr") != std::string::npos,
          "3621 AC1: victim storage depth helper declared (fiber.h #588)");
    CHECK(fh.find("thief thread must not read thread_local") != std::string::npos,
          "3621 AC1: thief-TLS prohibition documented at the helper");
    CHECK(fh.find("s.depth = aura_evaluator_mutation_stack_depth_from_ptr(") != std::string::npos,
          "3621 AC1: mutation_safety_snapshot depth loads via victim storage helper");
    CHECK(fh.find("depth from victim mutation_stack_storage_ (not thief TLS)") != std::string::npos,
          "3621 AC1: snapshot cite (depth from victim storage)");
    // The compiler-side slot surface must never leak into the serve/ steal path.
    CHECK(fh.find("mutation_boundary_depth_slot") == std::string::npos,
          "3621 AC1: fiber.h never reads the thief TLS depth slot");
    CHECK(ss.find("mutation_boundary_depth_slot") == std::string::npos,
          "3621 AC1: steal_safety.cpp never reads the thief TLS depth slot");
    // The transaction samples via the victim fiber object (never a TLS read).
    CHECK(ss.find("stolen->mutation_safety_snapshot()") != std::string::npos,
          "3621 AC6: steal_safety_transaction samples the victim snapshot");
}

static void ac3621_2_ticket_stamp_ok_only() {
    std::println("\n--- #3621 AC2: set_resume_safety_ticket reachable only on the Ok path ---");
    const auto ss = read_file("src/serve/steal_safety.cpp");
    // Enumerative: exactly one stamp site in the whole file, and it sits
    // AFTER the last RejectHard return in steal_safety_transaction (no
    // reject arm can reach it).
    std::size_t pos = 0;
    int stamps = 0;
    std::size_t stamp_pos = std::string::npos;
    while ((pos = ss.find("set_resume_safety_ticket(", pos)) != std::string::npos) {
        ++stamps;
        stamp_pos = pos;
        pos += 1;
    }
    CHECK(stamps == 1, "3621 AC2: exactly one ticket stamp site (sole-enqueue #2844)");
    const std::size_t txn = ss.find("StealSafetyDecision steal_safety_transaction(Fiber* stolen)");
    CHECK(txn != std::string::npos, "3621 AC6: steal_safety_transaction present");
    const std::size_t last_reject = ss.rfind("return StealSafetyDecision::RejectHard;");
    CHECK(last_reject != std::string::npos && stamp_pos != std::string::npos &&
              stamp_pos > last_reject && stamp_pos > txn,
          "3621 AC2: stamp site sits after the last RejectHard return (no reject arm stamps)");
    CHECK(ss.find("ticket stamp ONLY here") != std::string::npos,
          "3621 AC2: sole-enqueue cite at the stamp site");
    // Stamped ticket is the window's snapshot ticket (sample→enqueue equality).
    CHECK(ss.find("set_resume_safety_ticket(snap.ticket)") != std::string::npos,
          "3621 AC2: enqueue ticket equals the decision-window snapshot ticket");
}

static void ac3621_3_identity_and_soft_lifetime_cite() {
    std::println("\n--- #3621 AC3/AC4: victim-eval identity arms + Lifetime soft skip cited ---");
    const auto ss = read_file("src/serve/steal_safety.cpp");
    // All three evaluator-identity arms consult the victim's own identity
    // (#2727/#3617) — never the thief's g_current_fiber.
    int victim_reads = 0;
    for (std::size_t p = 0;
         (p = ss.find("aura_fiber_evaluator_id_for_steal_safety(stolen)", p)) != std::string::npos;
         p += 1)
        ++victim_reads;
    CHECK(victim_reads >= 3,
          "3621 AC6: GcDeferClear/EnvFrame/Lifetime arms key on victim evaluator id");
    CHECK(ss.find("g_current_fiber") == std::string::npos,
          "3621 AC1/AC6: steal_safety.cpp never consults thief g_current_fiber");
    // AC3: the Lifetime arm's Soft skip stays documented + cited (not a
    // missing arm — #2957/#3385).
    CHECK(ss.find("Soft: skip entirely (no loads)") != std::string::npos,
          "3621 AC3: Lifetime arm soft skip documented");
    CHECK(ss.find("Issue #3617: victim-eval keyed") != std::string::npos,
          "3621 AC6: #3617 identity cite at the residual arms");
    // AC4: no new counter / query key; no new invariant enum arm.
    CHECK(ss.find("g_3621_") == std::string::npos && ss.find("schema-3621") == std::string::npos,
          "3621 AC4: no new counter / query key");
    CHECK(read_file("tests/serve/test_issue_3621.cpp").empty(),
          "3621 AC5: no test_issue_3621.cpp (#81934 — extend existing suites)");
    CHECK(read_file("docs/design/3621-steal-identity-proof.md").empty(),
          "3621: no docs/design (per #1655)");
}

} // namespace

int run_test_steal_snapshot_hard_invariant() {
    std::println("=== Issue #2346: resume snapshot hard-invariant ===");

    // AC1 Soft — force SOFT=1
    {
        std::println("\n--- AC1: Soft mismatch → counter, continue ---");
        ::setenv("AURA_STEAL_SNAPSHOT_SOFT", "1", 1);
        ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
        CHECK(!aura::serve::is_steal_snapshot_hard_mode(), "AC1: Soft mode");
        const auto miss0 = aura_fiber_static_mutation_steal_snapshot_mismatch_total();
        const auto hard0 = aura_fiber_static_steal_snapshot_hard_fail_total();
        run_on_fiber([&]() {
            auto* fb = aura::serve::g_current_fiber;
            aura_evaluator_test_push_mutation_checkpoint();
            fb->set_yield_reason(YieldReason::Explicit);
            CHECK(fb->mutation_safety_snapshot_inconsistent(fb->mutation_safety_snapshot()),
                  "AC1: inconsistent");
            CHECK(fb->check_and_enforce_resume_snapshot_invariant(), "AC1: Soft continues");
            CHECK(aura_fiber_static_mutation_steal_snapshot_mismatch_total() > miss0,
                  "AC1: mismatch +1");
            CHECK(aura_fiber_static_steal_snapshot_hard_fail_total() == hard0,
                  "AC1: hard-fail unchanged");
            aura_evaluator_test_pop_mutation_checkpoint();
        });
        ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
    }

    // AC2 Hard via env
    {
        std::println("\n--- AC2: Hard mismatch → mark-failed ---");
        ::setenv("AURA_STEAL_SNAPSHOT_HARD", "1", 1);
        ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
        CHECK(aura::serve::is_steal_snapshot_hard_mode(), "AC2: Hard on");
        const auto miss0 = aura_fiber_static_mutation_steal_snapshot_mismatch_total();
        const auto hard0 = aura_fiber_static_steal_snapshot_hard_fail_total();
        run_on_fiber([&]() {
            auto* fb = aura::serve::g_current_fiber;
            aura_evaluator_test_push_mutation_checkpoint();
            fb->set_yield_reason(YieldReason::Explicit);
            CHECK(fb->mutation_safety_snapshot_inconsistent(fb->mutation_safety_snapshot()),
                  "AC2: inconsistent");
            CHECK(!fb->check_and_enforce_resume_snapshot_invariant(), "AC2: Hard stops resume");
            CHECK(fb->is_cancel_requested(), "AC2: cancel");
            CHECK(fb->state() == FiberState::Done, "AC2: Done");
            CHECK(aura_fiber_static_mutation_steal_snapshot_mismatch_total() > miss0,
                  "AC2: mismatch +1");
            CHECK(aura_fiber_static_steal_snapshot_hard_fail_total() > hard0, "AC2: hard-fail +1");
            aura_evaluator_test_pop_mutation_checkpoint();
        });
        ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
    }

    // AC3 happy path under Soft
    {
        std::println("\n--- AC3: happy path ---");
        ::setenv("AURA_STEAL_SNAPSHOT_SOFT", "1", 1);
        ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
        const auto hard0 = aura_fiber_static_steal_snapshot_hard_fail_total();
        const auto miss0 = aura_fiber_static_mutation_steal_snapshot_mismatch_total();
        run_on_fiber([&]() {
            auto* fb = aura::serve::g_current_fiber;
            fb->set_yield_reason(YieldReason::MutationBoundary);
            CHECK(fb->check_and_enforce_resume_snapshot_invariant(), "AC3: continue");
        });
        CHECK(aura_fiber_static_steal_snapshot_hard_fail_total() == hard0, "AC3: no hard-fail");
        CHECK(aura_fiber_static_mutation_steal_snapshot_mismatch_total() == miss0,
              "AC3: no mismatch");
        ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
    }

    // AC4 query
    {
        std::println("\n--- AC4: query schema-2346 ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(href(cs, "schema-2346") == 2346, "schema-2346");
        CHECK(href(cs, "issue-2346") == 2346, "issue-2346");
        CHECK(href(cs, "steal-snapshot-hard-wired") == 1, "wired");
        CHECK(href(cs, "steal-snapshot-mismatch-total") >= 0, "mismatch key");
        CHECK(href(cs, "steal-snapshot-hard-fail-total") >= 0, "hard-fail key");
        CHECK(href(cs, "schema-2184") == 2184, "2184 retained");
        CHECK(href(cs, "schema-2310") == 2310, "2310 retained");
    }

    // AC5 source-cite
    {
        std::println("\n--- AC5: source-cite ---");
        const auto fh = read_file("src/serve/fiber.h");
        const auto fc = read_file("src/serve/fiber.cpp");
        const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(fh.find("Issue #2346") != std::string::npos, "fiber.h #2346");
        CHECK(fh.find("AURA_STEAL_SNAPSHOT_HARD") != std::string::npos, "HARD env");
        CHECK(fh.find("check_and_enforce_resume_snapshot_invariant") != std::string::npos, "API");
        CHECK(fc.find("check_and_enforce_resume_snapshot_invariant") != std::string::npos,
              "resume uses helper");
        CHECK(fc.find("Issue #2346") != std::string::npos, "fiber.cpp #2346");
        CHECK(q.find("schema-2346") != std::string::npos, "query schema");
        CHECK(q.find("steal-snapshot-hard-fail-total") != std::string::npos, "query key");
    }

    // Issue #3621: enumerative machine proof — sample→enqueue cannot read
    // thief TLS (depth via victim storage; ticket stamps Ok-only; residual
    // arms key on victim evaluator id). Source-cite over #3072 style.
    ac3621_1_depth_victim_storage_identity();
    ac3621_2_ticket_stamp_ok_only();
    ac3621_3_identity_and_soft_lifetime_cite();

    std::println("\n=== #2346 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_steal_snapshot_hard_invariant();
}
#endif
