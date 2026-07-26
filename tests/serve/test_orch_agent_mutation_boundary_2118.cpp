// @category: unit
// @reason: Issue #2118 — register MutationBoundary depth + per-fiber stack
// for agent body visibility (steal #2115 + GC).
//
//   AC1: soft-boundary agent body → depth>0, is_at_mutation_boundary_safe false
//   AC2: mutation_boundary=false pure path → no soft enter (skip_pure++)
//   AC3: nested enter/release balanced (no depth leak)
//   AC4: query:orch-module-stats schema-2118 + entered metric
//   AC5: source wiring + soft vs full Guard on fiber

#include "test_harness.hpp"

#include "orch/agent_spawn.h"
#include "serve/fiber.h"
#include "serve/scheduler.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>

extern "C" int aura_orch_agent_body_try_acquire();
extern "C" int aura_orch_agent_body_try_acquire_ex(int register_soft_boundary);
extern "C" void aura_orch_agent_body_release_guard();
extern "C" std::size_t aura_evaluator_mutation_stack_depth_from_ptr(void*);

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::orch::AgentSpec;
using aura::orch::g_orch_module_stats;
using aura::orch::spawn_agent_with_mailbox;
using aura::serve::Fiber;
using aura::serve::Scheduler;
using aura::serve::YieldReason;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:orch-module-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

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

static void ac1_soft_boundary_visibility() {
    std::println("\n--- AC1: soft boundary → depth>0, steal not safe ---");
    // Wire evaluator for scheduler hooks
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval warm");

    Scheduler sched(2);
    std::atomic<bool> checked{false};
    const auto entered0 = g_orch_module_stats.orch_agent_boundary_entered_total.load();

    AgentSpec spec;
    spec.name = "mutate-agent";
    spec.mutation_boundary = true;
    spec.attach_mailbox = false;
    spec.body = [&]() {
        CHECK(aura::serve::g_current_fiber != nullptr, "on fiber");
        auto* f = aura::serve::g_current_fiber;
        CHECK(f->orch_agent_boundary_active(), "orch boundary active");
        const auto depth = aura_evaluator_mutation_stack_depth_from_ptr(f->mutation_stack_ptr());
        CHECK(depth >= 1, "per-fiber depth >= 1");
        // With soft boundary + depth>0, steal is not safe (#2115/#2118)
        CHECK(!f->is_at_mutation_boundary_safe(), "not steal-safe in mutation window");
        f->set_yield_reason(YieldReason::MutationBoundary);
        CHECK(!f->is_at_mutation_boundary_safe(), "MB yield + depth → not safe");
        checked.store(true);
    };
    auto h = spawn_agent_with_mailbox(sched, std::move(spec));
    CHECK(h.ok && h.fiber, "spawn ok");

    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!checked.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.stop();
    io.join();
    CHECK(checked.load(), "body ran");
    CHECK(g_orch_module_stats.orch_agent_boundary_entered_total.load() > entered0,
          "entered total++");
}

static void ac2_pure_reasoning_zero_cost() {
    std::println("\n--- AC2: pure reasoning mutation_boundary=false ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 0 0)").has_value(), "eval");

    Scheduler sched(2);
    std::atomic<bool> checked{false};
    const auto pure0 = g_orch_module_stats.orch_agent_boundary_skip_pure_total.load();
    const auto entered0 = g_orch_module_stats.orch_agent_boundary_entered_total.load();

    AgentSpec spec;
    spec.name = "pure-agent";
    spec.mutation_boundary = false; // AC2 zero-cost
    spec.attach_mailbox = false;
    spec.body = [&]() {
        auto* f = aura::serve::g_current_fiber;
        CHECK(f != nullptr, "fiber");
        CHECK(!f->orch_agent_boundary_active(), "no soft boundary");
        const auto depth = aura_evaluator_mutation_stack_depth_from_ptr(f->mutation_stack_ptr());
        // Depth may be 0 if stack never allocated, or unchanged
        CHECK(depth == 0 || !f->orch_agent_boundary_active(), "pure: no agent flag");
        CHECK(f->is_at_mutation_boundary_safe(), "pure: steal-safe (no window)");
        checked.store(true);
    };
    auto h = spawn_agent_with_mailbox(sched, std::move(spec));
    CHECK(h.ok, "spawn pure");
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!checked.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.stop();
    io.join();
    CHECK(checked.load(), "pure body ran");
    CHECK(g_orch_module_stats.orch_agent_boundary_skip_pure_total.load() > pure0, "skip pure++");
    CHECK(g_orch_module_stats.orch_agent_boundary_entered_total.load() == entered0,
          "no enter on pure");
}

static void ac3_nested_no_leak() {
    std::println("\n--- AC3: nested acquire/release no depth leak ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 2 2)").has_value(), "eval");
    Scheduler sched(1);
    std::atomic<bool> checked{false};
    sched.spawn([&]() {
        // Manual nested soft enter via C API
        CHECK(aura_orch_agent_body_try_acquire_ex(1) == 0, "outer acq");
        auto* f = aura::serve::g_current_fiber;
        CHECK(f && f->orch_agent_boundary_active(), "outer active");
        const auto d1 = aura_evaluator_mutation_stack_depth_from_ptr(f->mutation_stack_ptr());
        CHECK(d1 >= 1, "outer depth");
        CHECK(aura_orch_agent_body_try_acquire_ex(1) == 0, "inner acq");
        const auto d2 = aura_evaluator_mutation_stack_depth_from_ptr(f->mutation_stack_ptr());
        CHECK(d2 > d1, "nested depth grows");
        aura_orch_agent_body_release_guard();
        const auto d3 = aura_evaluator_mutation_stack_depth_from_ptr(f->mutation_stack_ptr());
        CHECK(d3 == d1, "after inner release depth restored");
        aura_orch_agent_body_release_guard();
        const auto d4 = aura_evaluator_mutation_stack_depth_from_ptr(f->mutation_stack_ptr());
        CHECK(d4 == 0, "fully released depth 0");
        CHECK(!f->orch_agent_boundary_active(), "flag cleared");
        checked.store(true);
    });
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!checked.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.stop();
    io.join();
    CHECK(checked.load(), "nested probe ran");
}

static void ac4_query() {
    std::println("\n--- AC4: query:orch-module-stats schema-2118 ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
    CHECK(href(cs, "schema-2118") == 2118, "schema-2118");
    CHECK(href(cs, "issue-2118") == 2118, "issue-2118");
    CHECK(href(cs, "orch-agent-soft-boundary-wired") == 1, "wired");
    CHECK(href(cs, "orch_agent_boundary_entered_total") >= 0, "entered key");
    CHECK(href(cs, "orch_agent_steal_skipped_boundary_total") >= 0, "steal skip key");
}

static void ac5_source() {
    std::println("\n--- AC5: source wiring ---");
    auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    auto as = read_file("src/orch/agent_spawn.h");
    auto fh = read_file("src/serve/fiber.h");
    auto wc = read_file("src/serve/worker.cpp");
    CHECK(fm.find("Issue #2118") != std::string::npos || fm.find("#2118") != std::string::npos,
          "fiber_mutation cites #2118");
    CHECK(fm.find("orch_soft_boundary_enter") != std::string::npos, "soft enter");
    CHECK(fm.find("aura_orch_agent_body_try_acquire_ex") != std::string::npos, "ex API");
    CHECK(as.find("mutation_boundary") != std::string::npos, "AgentSpec flag");
    CHECK(as.find("orch_agent_boundary_entered_total") != std::string::npos, "stats field");
    CHECK(fh.find("orch_agent_boundary_active") != std::string::npos, "fiber flag");
    CHECK(wc.find("aura_orch_note_agent_steal_skipped_boundary") != std::string::npos,
          "worker steal note");
    // Full Guard still avoided on fiber (#1881 retained)
    CHECK(fm.find("do not construct a full") != std::string::npos ||
              fm.find("Fiber stacks are small") != std::string::npos,
          "no full Guard on fiber");
}

} // namespace

int main() {
    std::println("=== Issue #2118: orch agent mutation boundary visibility ===");
    ac1_soft_boundary_visibility();
    ac2_pure_reasoning_zero_cost();
    ac3_nested_no_leak();
    ac4_query();
    ac5_source();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
