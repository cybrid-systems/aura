// @category: unit
// @reason: Issue #2483 — channel:send rendezvous (buffer_size=0) blocks
//          until a recv is posted; no buffer_size==0 wait short-circuit.
//
//   AC1: rendezvous send blocks until concurrent recv
//   AC2: buffered channel allows send without recv (size < buf)
//   AC3: source predicate / waiting_receivers / channels_mtx_ scope
//   AC4: gate wiring
//
// Issue #4700 — under --serve-async the channel primitives run inside a
// FIBER on a worker thread; the condvar wait froze the whole worker, so a
// peer fiber on the same worker could never run (deadlock).
//
//   AC5: fiber-parked channel handoff (no worker-thread deadlock)
//   AC6: source contract (cooperative channel park)
//   AC7: coverage manifest wiring

#include "test_harness.hpp"

#include "serve/fiber.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <thread>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::PrimFn;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::compiler::types::make_int;
using aura::compiler::types::make_string;
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

static std::optional<PrimFn> lookup_prim(Evaluator& ev, std::string_view name) {
    return ev.primitives().lookup(name);
}

// Invoke channel prims directly (bypass eval_mutex_) so send/recv can
// run on concurrent OS threads sharing one Evaluator.
static void ac1_rendezvous_blocks() {
    std::println("\n--- #2483 AC1: rendezvous send waits for recv ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    (void)cs.eval("1"); // warm registry

    auto create = lookup_prim(ev, "channel:create");
    auto send = lookup_prim(ev, "channel:send");
    auto recv = lookup_prim(ev, "channel:recv");
    CHECK(create && send && recv, "AC1: channel prims registered");
    if (!create || !send || !recv)
        return;

    auto id_v = (*create)({});
    CHECK(is_int(id_v), "AC1: create returns id");
    auto id = as_int(id_v);

    auto& sh = ev.string_heap_mut();
    auto sidx = sh.mint("hello-rendezvous");
    auto msg = make_string(static_cast<std::uint64_t>(sidx));

    std::atomic<bool> send_done{false};
    std::atomic<bool> send_ok{false};
    PrimFn send_fn = *send;
    std::thread sender([&] {
        auto r = send_fn({make_int(id), msg});
        send_ok.store(is_bool(r) && as_bool(r), std::memory_order_release);
        send_done.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(!send_done.load(std::memory_order_acquire),
          "AC1: send still blocked before recv (rendezvous)");

    auto got = (*recv)({make_int(id)});
    CHECK(is_string(got), "AC1: recv returns string");
    if (is_string(got)) {
        auto gi = as_string_idx(got);
        CHECK(gi < sh.size() && sh[gi] == "hello-rendezvous", "AC1: payload matches");
    }

    for (int i = 0; i < 50 && !send_done.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(send_done.load(std::memory_order_acquire), "AC1: send completed after recv");
    CHECK(send_ok.load(std::memory_order_acquire), "AC1: send returned #t");
    if (sender.joinable())
        sender.join();
}

static void ac2_buffered() {
    std::println("\n--- #2483 AC2: buffered send without recv ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    (void)cs.eval("1");
    auto create = lookup_prim(ev, "channel:create");
    auto send = lookup_prim(ev, "channel:send");
    auto try_recv = lookup_prim(ev, "channel:try-recv");
    CHECK(create && send && try_recv, "AC2: prims");
    if (!create || !send || !try_recv)
        return;

    auto id_v = (*create)({make_int(2)});
    CHECK(is_int(id_v), "AC2: create buf=2");
    auto id = as_int(id_v);

    auto& sh = ev.string_heap_mut();
    auto s0 = sh.mint("a");
    auto s1 = sh.mint("b");

    auto r0 = (*send)({make_int(id), make_string(static_cast<std::uint64_t>(s0))});
    auto r1 = (*send)({make_int(id), make_string(static_cast<std::uint64_t>(s1))});
    CHECK(is_bool(r0) && as_bool(r0), "AC2: first send ok without recv");
    CHECK(is_bool(r1) && as_bool(r1), "AC2: second send ok (buffer=2)");

    // Drain both messages without blocking — proves buffer held capacity 2.
    auto g0 = (*try_recv)({make_int(id)});
    auto g1 = (*try_recv)({make_int(id)});
    auto g2 = (*try_recv)({make_int(id)});
    CHECK(is_string(g0) && is_string(g1), "AC2: try-recv both buffered msgs");
    // Third try-recv empty (no more messages; size never exceeded buffer)
    CHECK(is_string(g2), "AC2: third try-recv returns string tag");
    if (is_string(g0) && is_string(g1)) {
        auto i0 = as_string_idx(g0), i1 = as_string_idx(g1);
        CHECK(i0 < sh.size() && sh[i0] == "a", "AC2: first payload a");
        CHECK(i1 < sh.size() && sh[i1] == "b", "AC2: second payload b");
    }
}

static void ac3_source() {
    std::println("\n--- #2483 AC3: source contracts ---");
    auto src = read_file("src/compiler/evaluator_primitives_messaging.cpp");
    CHECK(!src.empty(), "AC3: read messaging");
    CHECK(src.find("Issue #2483") != std::string::npos, "AC3: cites #2483");
    auto spos = src.find("channel:send");
    CHECK(spos != std::string::npos, "AC3: channel:send present");
    if (spos != std::string::npos) {
        // Issue #4700 widened the windows: the cooperative channel park
        // (fiber-level wait) added ~30 lines between the primitive
        // registrations, so the grep window has to span them.
        auto win = src.substr(spos, 4000);
        CHECK(win.find("waiting_receivers") != std::string::npos,
              "AC3: send uses waiting_receivers");
        CHECK(win.find("ch.buffer_size == 0 || ch.queue.size()") == std::string::npos,
              "AC3: no buggy wait Or short-circuit");
        CHECK(win.find("ch_ptr") != std::string::npos, "AC3: channels_mtx_ released before wait");
    }
    auto rpos = src.find("channel:recv");
    CHECK(rpos != std::string::npos, "AC3: channel:recv present");
    if (rpos != std::string::npos) {
        // Issue #4700: the fiber-cooperative receive path sits between the
        // registration and the host-thread wait — widen the window so the
        // "announces waiters" contract is still checked on the primitive.
        auto win = src.substr(rpos, 4000);
        CHECK(win.find("waiting_receivers") != std::string::npos, "AC3: recv announces waiters");
    }
    auto ixx = read_file("src/compiler/evaluator.ixx");
    CHECK(ixx.find("waiting_receivers") != std::string::npos, "AC3: Channel has waiting_receivers");
    CHECK(ixx.find("Issue #2483") != std::string::npos, "AC3: evaluator.ixx cites #2483");
}

static void ac4_gate() {
    std::println("\n--- #2483 AC4: gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    CHECK(build.find("check_channel_rendezvous_2483") != std::string::npos,
          "AC4: check script in build.py");
    CHECK(build.find("cmd_channel_rendezvous_coverage") != std::string::npos, "AC4: coverage cmd");
    CHECK(cmake.find("test_channel_rendezvous") != std::string::npos, "AC4: cmake test");
    CHECK(!read_file("scripts/coverage/checks/check_channel_rendezvous_2483.py").empty(),
          "AC4: check script exists");
}

// ── Issue #4700 AC5: fiber-parked channel handoff ────────────────
//
// Under --serve-async the channel primitives run inside a FIBER on a worker
// thread. Before #4700, channel:recv / channel:send blocked the whole
// worker on the channel condvar, so a peer fiber on the same worker could
// never run → deadlock.
//
// The test drives a rendezvous (buffer_size=0) channel on a 1-worker
// scheduler: whichever fiber runs first has to park at the FIBER level for
// the handoff, so a completed handoff proves the cooperative park. Without
// the fix the first fiber freezes the worker and the scheduler never
// finishes — the watchdog turns that into a FAIL instead of a hung binary.
static void ac5_fiber_park_no_deadlock() {
    std::println("\n--- #4700 AC5: fiber-parked channel handoff (no worker-thread deadlock) ---");
    // Heap-allocated on purpose: a deadlocked worker thread must not run
    // ~Scheduler (it would join the stuck worker). The watchdog below exits
    // the process on a deadlock, so the leak is only in the failing case.
    auto* sched = new aura::serve::Scheduler(1);
    CompilerService cs;
    auto& ev = cs.evaluator();
    (void)cs.eval("1"); // warm registry

    auto create = lookup_prim(ev, "channel:create");
    auto send = lookup_prim(ev, "channel:send");
    auto recv = lookup_prim(ev, "channel:recv");
    CHECK(create && send && recv, "AC5: channel prims registered");
    if (!create || !send || !recv)
        return;

    auto id_v = (*create)({}); // rendezvous: buffer_size = 0
    CHECK(is_int(id_v), "AC5: create returns id");
    auto id = as_int(id_v);

    auto& sh = ev.string_heap_mut();
    auto sidx = sh.mint("fiber-handoff");
    auto msg = make_string(static_cast<std::uint64_t>(sidx));

    std::atomic<bool> recv_done{false};
    std::atomic<bool> send_done{false};
    std::atomic<bool> recv_ok{false};
    std::atomic<bool> send_ok{false};
    PrimFn send_fn = *send;
    PrimFn recv_fn = *recv;

    sched->spawn([&] {
        auto r = recv_fn({make_int(id)});
        recv_ok.store(is_string(r), std::memory_order_release);
        recv_done.store(true, std::memory_order_release);
    });
    sched->spawn([&] {
        auto r = send_fn({make_int(id), msg});
        send_ok.store(is_bool(r) && as_bool(r), std::memory_order_release);
        send_done.store(true, std::memory_order_release);
    });

    std::atomic<bool> finished{false};
    std::thread runner([&] {
        sched->run();
        finished.store(true, std::memory_order_release);
    });
    runner.detach();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool scheduler_done = finished.load(std::memory_order_acquire);
    CHECK(scheduler_done, "AC5: scheduler finished (no worker-thread deadlock)");
    if (!scheduler_done) {
        std::println("AC5: FAIL — a channel fiber blocked its worker thread (#4700 deadlock)");
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(1);
    }
    CHECK(recv_done.load(std::memory_order_acquire), "AC5: receive fiber completed");
    CHECK(send_done.load(std::memory_order_acquire), "AC5: send fiber completed");
    CHECK(recv_ok.load(std::memory_order_acquire), "AC5: receive returned a string");
    CHECK(send_ok.load(std::memory_order_acquire), "AC5: send returned #t");
    delete sched;
}

// ── Issue #4700 AC6: source contract (cooperative channel park) ────
static void ac6_source_4700() {
    std::println("\n--- #4700 AC6: source contract (cooperative channel park) ---");
    auto src = read_file("src/compiler/evaluator_primitives_messaging.cpp");
    CHECK(!src.empty(), "AC6: read messaging");
    CHECK(src.find("Issue #4700") != std::string::npos, "AC6: cites #4700");
    CHECK(src.find("channel_park_fiber") != std::string::npos, "AC6: cooperative park helper");
    CHECK(src.find("channel_wake_waiters") != std::string::npos, "AC6: parked waiters are woken");
    CHECK(src.find("aura::serve::YieldReason::BlockingIO") != std::string::npos,
          "AC6: park yields with BlockingIO (fiber-level, not thread-level)");
    CHECK(src.find("CliBodyLockJoinGuard body_lock_guard") != std::string::npos,
          "AC6: denseness body mutex released across the park");
    // The channel mutex must never be held across the park (a parked fiber
    // may resume on another worker thread).
    auto park = src.find("channel_park_fiber(fiber);");
    CHECK(park != std::string::npos, "AC6: park call site present");
    if (park != std::string::npos) {
        auto win = src.substr(park > 900 ? park - 900 : 0, 900);
        CHECK(win.find("ul.unlock();") != std::string::npos,
              "AC6: channel mutex released before the park");
    }
    // The host-thread path (no fiber) must stay: the condvar wait is still
    // the fallback for CLI / stdin use.
    CHECK(src.find("ch.cv.wait(ul, [&]() { return ch.closed || !ch.queue.empty(); });") !=
              std::string::npos,
          "AC6: host-thread condvar wait preserved for the non-fiber path");
}

// ── Issue #4700 AC7: coverage manifest wiring ────────────────────
static void ac7_coverage_manifest() {
    std::println("\n--- #4700 AC7: coverage manifest ---");
    auto manifest = read_file("scripts/coverage/manifests/4700.json");
    CHECK(!manifest.empty(), "AC7: manifest exists");
    CHECK(manifest.find("\"issue\": 4700") != std::string::npos,
          "AC7: manifest declares issue 4700");
    CHECK(manifest.find("AC5") != std::string::npos, "AC7: manifest covers AC5");
    CHECK(manifest.find("AC6") != std::string::npos, "AC7: manifest covers AC6");
    CHECK(manifest.find("AC7") != std::string::npos, "AC7: manifest covers AC7");
}

} // namespace

int run_test_channel_rendezvous() {
    std::println("=== Issue #2483: channel rendezvous semantics ===");
    ac1_rendezvous_blocks();
    ac2_buffered();
    ac3_source();
    ac4_gate();
    // Issue #4700 — fiber-cooperative channel waits.
    ac5_fiber_park_no_deadlock();
    ac6_source_4700();
    ac7_coverage_manifest();
    std::println("\n=== #2483 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_channel_rendezvous();
}
#endif
