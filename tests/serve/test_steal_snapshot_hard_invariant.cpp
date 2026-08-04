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

    std::println("\n=== #2346 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_steal_snapshot_hard_invariant();
}
#endif
