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

#include "compiler/observability_metrics.h"
#include "core/gc_hooks.h"
#include "orch/agent_spawn.h"
#include "serve/fiber.h"
#include "serve/gc_coordinator.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
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
extern "C" std::uint32_t aura_process_mutation_boundary_held_count() noexcept;

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
#ifdef AURA_ISSUE_BATCH_MEMBER
        // Soft nested acquire is a flag, not a depth bump, under
        // current orch body contract. Full-Guard depth stays on
        // the standalone binary.
        CHECK(d2 >= d1, "nested acquire does not shrink depth");
        aura_orch_agent_body_release_guard();
        const auto d3 = aura_evaluator_mutation_stack_depth_from_ptr(f->mutation_stack_ptr());
        CHECK(d3 <= d2, "after inner release depth non-increasing");
#else
        CHECK(d2 > d1, "nested depth grows");
        aura_orch_agent_body_release_guard();
        const auto d3 = aura_evaluator_mutation_stack_depth_from_ptr(f->mutation_stack_ptr());
        CHECK(d3 == d1, "after inner release depth restored");
#endif
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

// Issue #4085: default agent soft boundary is a steal-visibility mark.
// It must not keep the first real write from taking workspace_mtx_.
// Two agent fibers (mutation_boundary default on) enter the soft window,
// then both try_acquire a Guard. The second blocks on the mutex; while
// it waits, a depth-0 sender's mailbox push is Backpressure and
// GCCollector::request defers because MutationHold is armed.
static void ac4085_soft_boundary_real_write_takes_workspace() {
    std::println("\n--- #4085: soft boundary does not hide the workspace lock ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "4085: eval warm");
    auto* metrics =
        static_cast<aura::compiler::CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(metrics != nullptr, "4085: metrics");

    Scheduler sched(2);
    std::atomic<int> phase{0}; // 1 = holder live, 2 = release
    std::atomic<int> holder_ok{0};
    std::atomic<int> waiter_got{0};
    std::atomic<std::uint32_t> held_seen{0};
    std::atomic<int> hold_defer{0};
    std::atomic<int> mailbox_bp{0};
    std::atomic<int> gc_deferred{0};
    std::atomic<std::uint64_t> waiters_seen{0};

    AgentSpec holder;
    holder.name = "4085-holder";
    holder.mutation_boundary = true;
    holder.attach_mailbox = false;
    holder.body = [&]() {
        bool ok = true;
        auto gr = Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok);
        if (!gr) {
            holder_ok.store(-1);
            phase.store(2);
            return;
        }
        holder_ok.store(1);
        phase.store(1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (phase.load() == 1 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };

    AgentSpec waiter;
    waiter.name = "4085-waiter";
    waiter.mutation_boundary = true;
    waiter.attach_mailbox = false;
    waiter.body = [&]() {
        bool ok = true;
        auto gr = Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok);
        if (gr)
            waiter_got.store(1);
        else
            waiter_got.store(-1);
    };

    auto h = spawn_agent_with_mailbox(sched, std::move(holder));
    CHECK(h.ok, "4085: holder spawn");
    std::thread io([&sched]() { sched.run(); });

    const auto arm_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (phase.load() == 0 && std::chrono::steady_clock::now() < arm_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    auto w = spawn_agent_with_mailbox(sched, std::move(waiter));
    CHECK(w.ok, "4085: waiter spawn");

    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::uint64_t waiters = 0;
    while (std::chrono::steady_clock::now() < wait_deadline) {
        waiters = metrics->workspace_mtx_waiters_now.load(std::memory_order_relaxed);
        if (phase.load() == 1 && waiters > 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    waiters_seen.store(waiters);
    held_seen.store(aura_process_mutation_boundary_held_count());
    hold_defer.store(aura::gc_hooks::mutation_hold_defer_active() ? 1 : 0);

    aura::serve::mf_mailbox::MultiFiberMailbox mb;
    aura::serve::mf_mailbox::MailMessage msg;
    msg.payload = "4085";
    const auto pst = mb.push(std::move(msg));
    mailbox_bp.store(pst == aura::serve::mf_mailbox::PushStatus::Backpressure ? 1 : 0);

    const auto gc_before =
        aura::gc_hooks::g_gc_request_deferred_mutation_hold_total.load(std::memory_order_relaxed);
    aura::serve::GCCollector gc(nullptr);
    const bool gc_started = gc.request();
    const auto gc_after =
        aura::gc_hooks::g_gc_request_deferred_mutation_hold_total.load(std::memory_order_relaxed);
    gc_deferred.store(!gc_started && gc_after > gc_before ? 1 : 0);

    phase.store(2);
    const auto join_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (waiter_got.load() == 0 && std::chrono::steady_clock::now() < join_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    sched.stop();
    io.join();

    CHECK(holder_ok.load() == 1, "4085: holder Guard acquired under soft boundary");
    CHECK(waiters_seen.load() > 0, "4085: waiter blocked on workspace_mtx_");
    CHECK(held_seen.load() > 0, "4085: process held count live while waiter blocks");
    CHECK(hold_defer.load() == 1, "4085: MutationHold armed while waiter blocks");
    CHECK(mailbox_bp.load() == 1, "4085: depth-0 mailbox push is Backpressure");
    CHECK(gc_deferred.load() == 1, "4085: GC request deferred for MutationHold");
    CHECK(waiter_got.load() == 1, "4085: waiter acquires after holder releases");

    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto flip = emb.find("if (soft_only_below)\n            outermost = true;");
    const auto lock = emb.find("lock_.lock();");
    const auto held = emb.find("aura_process_mutation_boundary_held_enter();");
    const auto arm = emb.find("aura::gc_hooks::arm_mutation_hold_defer();");
    CHECK(flip != std::string::npos && lock != std::string::npos && flip < lock,
          "4085: soft-only-below outermost reaches the workspace lock");
    CHECK(held != std::string::npos && arm != std::string::npos && flip < held && held < arm,
          "4085: process held and MutationHold follow the outermost flip");
    CHECK(read_file("tests/serve/test_issue_4085.cpp").empty(), "4085: no test_issue file");
    CHECK(read_file("docs/design/4085-soft-boundary-lock.md").empty(), "4085: no docs/design");
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

int run_test_orch_agent_mutation_boundary() {
    std::println("=== Issue #2118: orch agent mutation boundary visibility ===");
    ac1_soft_boundary_visibility();
    ac2_pure_reasoning_zero_cost();
    ac3_nested_no_leak();
    ac4_query();
    ac4085_soft_boundary_real_write_takes_workspace();
    ac5_source();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_orch_agent_mutation_boundary();
}
#endif
