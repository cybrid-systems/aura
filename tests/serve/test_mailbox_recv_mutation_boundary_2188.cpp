// @category: unit
// @reason: Issue #2188 — forbid blocking MultiFiberMailbox::recv /
// Fiber::yield while MutationBoundary is live (depth>0 or held).
//
//   AC1: under live Guard (depth≥1), recv(wait=true) does not yield;
//        returns empty; recv_rejected_in_mutation_boundary bumps
//   AC2: depth==0 path unchanged (non-blocking + message delivery work)
//   AC3: fanout / push / linear-viol filter unchanged (source + smoke)
//   AC4: fiber holds Guard, recv → no yield/deadlock; metric ≥1
//   AC5: source-cite gate next to #2010 linear-viol hot-path comment

#include "test_harness.hpp"

#include "serve/fiber.h"
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

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

extern "C" std::size_t aura_evaluator_mutation_boundary_depth();
extern "C" int aura_evaluator_mutation_boundary_held();

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::Scheduler;
using aura::serve::YieldReason;
using aura::serve::mf_mailbox::g_mf_mailbox_stats;
using aura::serve::mf_mailbox::MailMessage;
using aura::serve::mf_mailbox::MultiFiberMailbox;
using aura::serve::mf_mailbox::PushStatus;
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

static void ac1_recv_rejected_under_guard() {
    std::println("\n--- AC1: recv under Guard → empty + counter, no yield ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm eval");
    auto& ev = cs.evaluator();

    MultiFiberMailbox mb(/*high_water=*/16);
    const auto rej0 =
        g_mf_mailbox_stats.recv_rejected_in_mutation_boundary.load(std::memory_order_relaxed);

    bool ok = true;
    {
        // Outermost Guard → depth ≥ 1 (and held).
        auto guard_r = Evaluator::MutationBoundaryGuard::try_acquire(ev, /*pending=*/1, &ok);
        CHECK(guard_r.has_value(), "try_acquire Guard");
        auto guard = std::move(*guard_r);
        CHECK(aura_evaluator_mutation_boundary_depth() > 0, "depth > 0 under Guard");
        CHECK(aura_evaluator_mutation_boundary_held() != 0 ||
                  aura_evaluator_mutation_boundary_depth() > 0,
              "boundary live");

        // Blocking-style recv must not park — Policy A empty return.
        auto msg = mb.recv(/*wait=*/true, /*timeout_ms=*/-1);
        CHECK(!msg.has_value(), "recv under Guard returns empty (Policy A)");
        const auto rej1 =
            g_mf_mailbox_stats.recv_rejected_in_mutation_boundary.load(std::memory_order_relaxed);
        CHECK(rej1 > rej0, "recv_rejected_in_mutation_boundary bumped");
        // Second call also gated (idempotent reject).
        (void)mb.recv(true, -1);
        CHECK(g_mf_mailbox_stats.recv_rejected_in_mutation_boundary.load(
                  std::memory_order_relaxed) > rej1,
              "reject counter grows on repeated gated recv");
    }
    CHECK(aura_evaluator_mutation_boundary_depth() == 0, "depth 0 after Guard exit");
}

static void ac2_depth0_unchanged() {
    std::println("\n--- AC2: depth==0 path unchanged ---");
    CHECK(aura_evaluator_mutation_boundary_depth() == 0, "depth 0 start");
    MultiFiberMailbox mb(/*high_water=*/16);

    // Non-blocking empty.
    auto empty = mb.recv(/*wait=*/false, /*timeout_ms=*/0);
    CHECK(!empty.has_value(), "try path empty when queue empty");

    // try_recv alias.
    auto empty2 = mb.try_recv();
    CHECK(!empty2.has_value(), "try_recv empty");

    // Push + pop delivery.
    MailMessage m;
    m.from_fiber = 1;
    m.to_fiber = 0;
    m.payload = "hello-2188";
    CHECK(mb.push(m) == PushStatus::Ok, "push ok");
    auto got = mb.recv(/*wait=*/false, /*timeout_ms=*/0);
    CHECK(got.has_value() && got->payload == "hello-2188", "depth0 delivery works");

    // With message already present, wait=true also returns without park.
    m.payload = "second";
    CHECK(mb.push(m) == PushStatus::Ok, "push second");
    auto got2 = mb.recv(/*wait=*/true, /*timeout_ms=*/50);
    CHECK(got2.has_value() && got2->payload == "second", "depth0 wait with message");
}

static void ac3_push_linear_unchanged() {
    std::println("\n--- AC3: push / linear-viol filter unchanged ---");
    MultiFiberMailbox mb(/*high_water=*/4);
    const auto lchk0 = g_mf_mailbox_stats.linear_checks.load(std::memory_order_relaxed);
    const auto lviol0 = g_mf_mailbox_stats.linear_violations.load(std::memory_order_relaxed);

    MailMessage good;
    good.payload = "normal-payload";
    CHECK(mb.push(good) == PushStatus::Ok, "normal push ok");

    MailMessage bad;
    bad.payload = "linear-viol:agent-claim";
    CHECK(mb.push(bad) == PushStatus::Closed, "linear-viol still returns Closed");
    CHECK(g_mf_mailbox_stats.linear_checks.load(std::memory_order_relaxed) > lchk0,
          "linear_checks bumped");
    CHECK(g_mf_mailbox_stats.linear_violations.load(std::memory_order_relaxed) > lviol0,
          "linear_violations bumped");
    // Source contract for #2010 still present.
    auto hdr = read_file("src/serve/multi_fiber_mailbox.h");
    CHECK(hdr.find("kLinearViolPrefix") != std::string::npos, "linear-viol prefix retained");
    CHECK(hdr.find("reject_if_linear_viol") != std::string::npos ||
              hdr.find("is_linear_viol_payload") != std::string::npos,
          "linear filter helpers retained");
}

static void ac4_fiber_holds_guard_recv() {
    std::println("\n--- AC4: fiber + Guard + recv → no yield, metric ≥1 ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto& ev = cs.evaluator();

    Scheduler sched(2);
    MultiFiberMailbox mb(/*high_water=*/8);
    std::atomic<bool> done{false};
    std::atomic<bool> no_yield{true};
    std::atomic<std::uint64_t> rej_delta{0};

    const auto rej0 =
        g_mf_mailbox_stats.recv_rejected_in_mutation_boundary.load(std::memory_order_relaxed);

    sched.spawn([&]() {
        bool ok = true;
        auto guard_r = Evaluator::MutationBoundaryGuard::try_acquire(ev, /*pending=*/1, &ok);
        if (!guard_r) {
            done.store(true);
            return;
        }
        auto guard = std::move(*guard_r);
        CHECK(aura_evaluator_mutation_boundary_depth() > 0, "fiber depth > 0");
        // Blocking recv must return empty without parking this fiber.
        auto before_state = aura::serve::g_current_fiber ? aura::serve::g_current_fiber->state()
                                                         : aura::serve::FiberState::Running;
        auto msg = mb.recv(true, -1);
        auto after_state = aura::serve::g_current_fiber ? aura::serve::g_current_fiber->state()
                                                        : aura::serve::FiberState::Running;
        CHECK(!msg.has_value(), "fiber recv under Guard empty");
        // Should still be Running (not left Waiting).
        CHECK(after_state == aura::serve::FiberState::Running || after_state == before_state,
              "fiber not left Waiting after gated recv");
        const auto rej1 =
            g_mf_mailbox_stats.recv_rejected_in_mutation_boundary.load(std::memory_order_relaxed);
        rej_delta.store(rej1 - rej0);
        no_yield.store(rej1 > rej0);
        done.store(true);
    });

    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.stop();
    io.join();
    CHECK(done.load(), "fiber body completed");
    CHECK(no_yield.load() && rej_delta.load() >= 1, "metric ≥1 after fiber gated recv");
}

static void ac5_source_and_metrics() {
    std::println("\n--- AC5: source cite + query schema-2188 ---");
    auto hdr = read_file("src/serve/multi_fiber_mailbox.h");
    CHECK(hdr.find("Issue #2188") != std::string::npos || hdr.find("#2188") != std::string::npos,
          "mailbox cites #2188");
    CHECK(hdr.find("recv_rejected_in_mutation_boundary") != std::string::npos,
          "reject counter present");
    CHECK(hdr.find("aura_evaluator_mutation_boundary_depth") != std::string::npos,
          "depth probe in recv");
    // Gate next to #2010 linear-viol contract (comment in recv / header).
    CHECK(hdr.find("#2010") != std::string::npos, "still cites #2010");
    CHECK(hdr.find("try_recv") != std::string::npos, "try_recv alias");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    // Force at least one reject so stats surface is non-zero.
    bool ok = true;
    {
        auto guard_r =
            Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), /*pending=*/1, &ok);
        CHECK(guard_r.has_value(), "guard for stats");
        auto guard = std::move(*guard_r);
        MultiFiberMailbox mb;
        (void)mb.recv(true, -1);
    }
    CHECK(href(cs, "schema-2188") == 2188, "schema-2188");
    CHECK(href(cs, "issue-2188") == 2188, "issue-2188");
    CHECK(href(cs, "recv-boundary-gate-wired") == 1, "gate wired");
    CHECK(href(cs, "recv-rejected-in-mutation-boundary") >= 1, "reject visible on query");
    // Lineage retained.
    CHECK(href(cs, "schema-2010") == 2010, "schema-2010 retained");
    CHECK(href(cs, "schema-1881") == 1881, "schema-1881 retained");
}

} // namespace

int main() {
    std::println("=== Issue #2188: mailbox recv gate under MutationBoundary ===");
    ac1_recv_rejected_under_guard();
    ac2_depth0_unchanged();
    ac3_push_linear_unchanged();
    ac4_fiber_holds_guard_recv();
    ac5_source_and_metrics();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
