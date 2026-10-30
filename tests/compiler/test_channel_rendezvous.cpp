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
//
// Issue #4744 — channel:send of a NUMBER from a fiber body never arrived
// (the receiver blocked in channel:recv forever) because a non-string payload
// was rejected with a primitive error VALUE that a body drops.
//
//   AC8: fixnum / bool / void / float payloads are delivered (FIFO too)
//   AC9: an undeliverable payload (a closure, at any depth) is rejected with
//        the kind named, nothing queued — #4763 superseded "every Ref" for
//        list / vector / hash, which are now deep-copied
//   AC10: source contract + manifest
//
// Issue #4763 — a list / vector / hash sent on a channel is deep-copied: the
// sender serialises it and the receiver rebuilds it in its own heap, so a
// sender-side mutation after the send is invisible to the receiver.
//
//   AC11: structured payloads cross a fiber boundary as copies

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
using aura::compiler::types::as_float;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_error;
using aura::compiler::types::is_float;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::compiler::types::is_vector;
using aura::compiler::types::is_void;
using aura::compiler::types::make_bool;
using aura::compiler::types::make_float;
using aura::compiler::types::make_int;
using aura::compiler::types::make_string;
using aura::compiler::types::make_void;
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
    // Issue #4763: anchor on the REGISTRATION, not the bare name — the
    // file's leading prose now mentions "channel:send" (deep-copy
    // comment) hundreds of lines before the lambda, so anchoring on the
    // first occurrence would window over a comment and miss the send path.
    auto spos = src.find(R"(add("channel:send")");
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
    // Same registration anchor as send (see above).
    auto rpos = src.find(R"(add("channel:recv")");
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


// ── Issue #4744: immediate (number / bool / void / float) payloads ──
// A non-string payload used to be rejected with a primitive error VALUE, which
// a fiber body drops on the floor: the paired channel:recv then blocked
// forever (the filed repro: 8 fibers send the integer 1, the coordinator hangs
// with rc=124 while the same program sending "1" completes). An immediate
// value is now delivered as itself, and a Ref payload (pair / closure / ...)
// is rejected LOUDLY with the offending kind named.
static void ac8_immediate_payload_4744() {
    std::println("\n--- #4744 AC8: immediate payloads are delivered ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    (void)cs.eval("1");
    auto create = lookup_prim(ev, "channel:create");
    auto send = lookup_prim(ev, "channel:send");
    auto recv = lookup_prim(ev, "channel:recv");
    CHECK(create && send && recv, "AC8: channel prims registered");
    if (!create || !send || !recv)
        return;
    auto id = as_int((*create)({make_int(64)}));

    auto s_int = (*send)({make_int(id), make_int(7)});
    CHECK(is_bool(s_int) && as_bool(s_int), "AC8: send of a fixnum returns #t");
    auto g_int = (*recv)({make_int(id)});
    CHECK(is_int(g_int) && as_int(g_int) == 7,
          "AC8: recv delivers the fixnum itself (was: error value + blocked recv)");

    auto s_bool = (*send)({make_int(id), make_bool(true)});
    CHECK(is_bool(s_bool) && as_bool(s_bool), "AC8: send of #t returns #t");
    auto g_bool = (*recv)({make_int(id)});
    CHECK(is_bool(g_bool) && as_bool(g_bool), "AC8: recv delivers #t");

    auto s_void = (*send)({make_int(id), make_void()});
    CHECK(is_bool(s_void) && as_bool(s_void), "AC8: send of void returns #t");
    auto g_void = (*recv)({make_int(id)});
    CHECK(is_void(g_void), "AC8: recv delivers void");

    auto s_float = (*send)({make_int(id), make_float(1.5)});
    CHECK(is_bool(s_float) && as_bool(s_float), "AC8: send of a float returns #t");
    auto g_float = (*recv)({make_int(id)});
    CHECK(is_float(g_float) && as_float(g_float) == 1.5, "AC8: recv delivers the float");

    // FIFO order is preserved across payload kinds.
    (void)(*send)({make_int(id), make_int(1)});
    (void)(*send)({make_int(id), make_float(2.5)});
    auto f1 = (*recv)({make_int(id)});
    auto f2 = (*recv)({make_int(id)});
    CHECK(is_int(f1) && as_int(f1) == 1 && is_float(f2) && as_float(f2) == 2.5,
          "AC8: mixed immediate payloads keep FIFO order");
}

// AC9: an undeliverable payload is named, and nothing is queued.
//
// Issue #4763 superseded the "every Ref payload is rejected" rule that AC9
// was written against: a list / vector / hash is now deep-copied on
// channel:send (AC11), so those kinds ARE deliverable. What still cannot
// cross a body boundary is a value whose identity is body-local — a closure.
// It must still be rejected LOUDLY with the offending kind named (the #4494
// lesson: a silent drop turns a type mismatch into a deadlock), and nothing
// may be queued by the rejected send.
static void ac9_undeliverable_payload_4744() {
    std::println("\n--- #4744 AC9: undeliverable payload is named, not dropped ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    (void)cs.eval("1");
    auto create = lookup_prim(ev, "channel:create");
    auto send = lookup_prim(ev, "channel:send");
    auto try_recv = lookup_prim(ev, "channel:try-recv");
    CHECK(create && send && try_recv, "AC9: channel prims registered");
    if (!create || !send || !try_recv)
        return;
    auto id = as_int((*create)({make_int(4)}));
    auto closure = cs.eval("(lambda (x) x)");
    CHECK(closure && is_closure(*closure), "AC9: a closure payload value is available");
    if (!closure)
        return;
    auto rejected = (*send)({make_int(id), *closure});
    CHECK(is_error(rejected), "AC9: send of a closure returns an error value");
    // The message names the payload kind, so a caller can act on it.
    auto names_kind = [&ev](const char* needle) {
        auto& heap = ev.string_heap_mut();
        for (std::size_t i = 0; i < heap.size(); ++i) {
            if (heap[i].find(needle) != std::string::npos)
                return true;
        }
        return false;
    };
    CHECK(names_kind("unsupported payload type (closure)"),
          "AC9: the reject names the payload kind (closure)");
    // The same holds at depth: a closure nested inside a structured payload is
    // still body-local state, so the reject names the nesting instead of
    // byte-serialising it.
    auto nested = cs.eval("(list 1 (lambda (y) y))");
    CHECK(nested && is_pair(*nested), "AC9: a nested-closure payload value is available");
    if (nested) {
        auto nested_rejected = (*send)({make_int(id), *nested});
        CHECK(is_error(nested_rejected),
              "AC9: send of a list holding a closure returns an error value");
        CHECK(names_kind("unsupported payload type (closure nested in a list/vector/hash)"),
              "AC9: the nested reject names the payload kind and where it sat");
    }
    // Nothing was enqueued: the next payload a receiver sees is the one sent
    // AFTER the rejected send.
    (void)(*send)({make_int(id), make_int(11)});
    auto after = (*try_recv)({make_int(id)});
    CHECK(is_int(after) && as_int(after) == 11, "AC9: nothing was queued by the rejected send");
}

// AC10: source contract (payload envelope + immediate pass-through).
static void ac10_source_4744() {
    std::println("\n--- #4744 AC10: source contract (payload envelope) ---");
    auto src = read_file("src/compiler/evaluator_primitives_messaging.cpp");
    CHECK(!src.empty(), "AC10: read messaging");
    CHECK(src.find("Issue #4744") != std::string::npos, "AC10: cites #4744");
    CHECK(src.find("channel_message_to_value") != std::string::npos,
          "AC10: an immediate payload is turned back into a value on recv");
    CHECK(src.find("unsupported payload type") != std::string::npos,
          "AC10: an undeliverable payload is named");
    auto hdr = read_file("src/compiler/evaluator.ixx");
    CHECK(hdr.find("struct ChannelMessage") != std::string::npos,
          "AC10: the channel queue carries a ChannelMessage envelope");
    auto manifest = read_file("scripts/coverage/manifests/4744.json");
    CHECK(!manifest.empty(), "AC10: manifest exists");
    CHECK(manifest.find("\"issue\": 4744") != std::string::npos,
          "AC10: manifest declares issue 4744");
}

// ── Issue #4763: structured payloads are deep-copied ───────────────
// A list / vector / hash is serialised on channel:send and rebuilt in the
// receiving body, so a sender-side mutation AFTER the send cannot reach the
// receiver. Before #4763 a structured payload was a Ref naming the sender's
// heap, so it was rejected outright (the rule AC9 was written against).
static void ac11_deep_copy_4763() {
    std::println("\n--- #4763 AC11: structured payloads are deep-copied ---");
    // Heap-allocated like AC5: a stuck worker must not run ~Scheduler.
    // One worker: a multi-worker Scheduler refuses to start without the
    // production security bootstrap, and latching that process-global state
    // from a test file would leak into the other batch members. The receive
    // is a fiber-level read of an already-queued message, so one worker is
    // enough to prove the copy crosses a fiber boundary.
    auto* sched = new aura::serve::Scheduler(1);
    CompilerService cs;
    auto& ev = cs.evaluator();
    (void)cs.eval("1"); // warm registry

    auto create = lookup_prim(ev, "channel:create");
    auto send = lookup_prim(ev, "channel:send");
    auto recv = lookup_prim(ev, "channel:recv");
    auto car = lookup_prim(ev, "car");
    auto cdr = lookup_prim(ev, "cdr");
    auto set_car = lookup_prim(ev, "set-car!");
    auto vec_ref = lookup_prim(ev, "vector-ref");
    auto vec_len = lookup_prim(ev, "vector-length");
    auto vec_set = lookup_prim(ev, "vector-set!");
    auto hash_make = lookup_prim(ev, "hash");
    auto hash_ref = lookup_prim(ev, "hash-ref");
    auto hash_set = lookup_prim(ev, "hash-set!");
    CHECK(create && send && recv && car && cdr && set_car && vec_ref && vec_len && vec_set &&
              hash_make && hash_ref && hash_set,
          "AC11: channel + structure prims registered");
    if (!create || !send || !recv || !car || !cdr || !set_car || !vec_ref || !vec_len || !vec_set ||
        !hash_make || !hash_ref || !hash_set)
        return;

    // Buffered, so the send never has to park: the deep copy is taken when
    // the message is enqueued, before the receiver ever runs.
    auto id_v = (*create)({make_int(8)});
    CHECK(is_int(id_v), "AC11: create returns id");
    if (!is_int(id_v))
        return;
    auto id = as_int(id_v);

    auto& sh = ev.string_heap_mut();
    const auto key_k = make_string(static_cast<std::uint64_t>(sh.mint("k")));
    const auto key_n = make_string(static_cast<std::uint64_t>(sh.mint("n")));

    // Sender side: a nested list, a nested vector, and a hash holding both.
    auto list_opt = cs.eval("(list 1 (list 2 3) 4)");
    auto vec_opt = cs.eval("(vector 1 (vector 2 3) 4)");
    CHECK(list_opt && is_pair(*list_opt), "AC11: a list payload value is available");
    CHECK(vec_opt && is_vector(*vec_opt), "AC11: a vector payload value is available");
    if (!list_opt || !vec_opt)
        return;
    const EvalValue list_v = *list_opt;
    const EvalValue vec_v = *vec_opt;
    EvalValue hash_v = (*hash_make)({});
    (void)(*hash_set)({hash_v, key_k, list_v});
    (void)(*hash_set)({hash_v, key_n, make_int(7)});
    CHECK(is_hash(hash_v), "AC11: a hash payload value is available");

    const auto sent_list = (*send)({make_int(id), list_v});
    const auto sent_vec = (*send)({make_int(id), vec_v});
    const auto sent_hash = (*send)({make_int(id), hash_v});
    CHECK(is_bool(sent_list) && as_bool(sent_list),
          "AC11: a list payload is delivered, not rejected");
    CHECK(is_bool(sent_vec) && as_bool(sent_vec),
          "AC11: a vector payload is delivered, not rejected");
    CHECK(is_bool(sent_hash) && as_bool(sent_hash),
          "AC11: a hash payload is delivered, not rejected");

    // Mutate the SENDER side in place, after the sends.
    const auto list_inner = (*car)({(*cdr)({list_v})});
    CHECK(is_pair(list_inner), "AC11: the list nests a pair");
    if (is_pair(list_inner))
        (void)(*set_car)({list_inner, make_int(99)});
    const auto vec_nested = (*vec_ref)({vec_v, make_int(1)});
    CHECK(is_vector(vec_nested), "AC11: the vector nests a vector");
    if (is_vector(vec_nested))
        (void)(*vec_set)({vec_nested, make_int(0), make_int(99)});
    (void)(*vec_set)({vec_v, make_int(2), make_int(99)});
    (void)(*hash_set)({hash_v, key_n, make_int(99)});
    // Guard against a vacuous row: the sender's own view MUST show the writes
    // (otherwise "the receiver is unaffected" would prove nothing).
    const auto live_inner_car = is_pair(list_inner) ? (*car)({list_inner}) : make_void();
    const auto live_vec_2 = (*vec_ref)({vec_v, make_int(2)});
    const auto live_hash_n = (*hash_ref)({hash_v, key_n});
    CHECK(is_int(live_inner_car) && as_int(live_inner_car) == 99,
          "AC11: the sender-side list really was mutated");
    CHECK(is_int(live_vec_2) && as_int(live_vec_2) == 99,
          "AC11: the sender-side vector really was mutated");
    CHECK(is_int(live_hash_n) && as_int(live_hash_n) == 99,
          "AC11: the sender-side hash really was mutated");

    // The receiver runs on its own fiber and reads the queued copies.
    struct Observed {
        bool list_shape = false;
        bool list_copy = false;
        bool vec_shape = false;
        bool vec_copy = false;
        bool hash_value = false;
        bool hash_copy = false;
        bool hash_list_value = false;
    };
    // Written only by the fiber; read after the scheduler thread joins, so a
    // plain struct is ordered by that join.
    Observed obs;
    PrimFn recv_fn = *recv;
    PrimFn car_fn = *car;
    PrimFn cdr_fn = *cdr;
    PrimFn vref_fn = *vec_ref;
    PrimFn vlen_fn = *vec_len;
    PrimFn href_fn = *hash_ref;
    const EvalValue fiber_key_k = key_k;
    const EvalValue fiber_key_n = key_n;
    sched->spawn([&] {
        const auto got_list = recv_fn({make_int(id)});
        const auto got_vec = recv_fn({make_int(id)});
        const auto got_hash = recv_fn({make_int(id)});
        if (is_pair(got_list)) {
            const auto e1 = car_fn({got_list});
            const auto rest = cdr_fn({got_list});
            const auto e2 = is_pair(rest) ? car_fn({rest}) : make_void();
            const auto rest2 = is_pair(rest) ? cdr_fn({rest}) : make_void();
            const auto e3 = is_pair(rest2) ? car_fn({rest2}) : make_void();
            obs.list_shape =
                is_int(e1) && as_int(e1) == 1 && is_pair(e2) && is_int(e3) && as_int(e3) == 4;
            if (is_pair(e2)) {
                const auto inner_car = car_fn({e2});
                const auto inner_tail = cdr_fn({e2});
                const auto inner_cadr = is_pair(inner_tail) ? car_fn({inner_tail}) : make_void();
                obs.list_copy = is_int(inner_car) && as_int(inner_car) == 2 && is_int(inner_cadr) &&
                                as_int(inner_cadr) == 3;
            }
        }
        if (is_vector(got_vec) && is_int(vlen_fn({got_vec})) && as_int(vlen_fn({got_vec})) == 3) {
            const auto v0 = vref_fn({got_vec, make_int(0)});
            const auto v1 = vref_fn({got_vec, make_int(1)});
            const auto v2 = vref_fn({got_vec, make_int(2)});
            obs.vec_shape = is_int(v0) && as_int(v0) == 1 && is_int(v2) && as_int(v2) == 4;
            if (is_vector(v1)) {
                const auto n0 = vref_fn({v1, make_int(0)});
                const auto n1 = vref_fn({v1, make_int(1)});
                obs.vec_copy = is_int(n0) && as_int(n0) == 2 && is_int(n1) && as_int(n1) == 3;
            }
        }
        if (is_hash(got_hash)) {
            const auto hn = href_fn({got_hash, fiber_key_n});
            const auto hk = href_fn({got_hash, fiber_key_k});
            obs.hash_value = is_int(hn) && as_int(hn) == 7;
            obs.hash_copy =
                obs.hash_value && is_pair(hk) && is_int(car_fn({hk})) && as_int(car_fn({hk})) == 1;
            const auto hk_tail = is_pair(hk) ? cdr_fn({hk}) : make_void();
            const auto hk_cadr = is_pair(hk_tail) ? car_fn({hk_tail}) : make_void();
            // The hash value is the nested list (1 (2 3) 4): its second
            // element is the inner list, whose car the sender set to 99.
            obs.hash_list_value =
                is_pair(hk_cadr) && is_int(car_fn({hk_cadr})) && as_int(car_fn({hk_cadr})) == 2;
        }
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
    CHECK(scheduler_done, "AC11: the receiving fiber finished");
    if (!scheduler_done) {
        std::println("AC11: FAIL — the receiving fiber never returned (#4763)");
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(1);
    }
    CHECK(obs.list_shape, "AC11: the received list has the sender's pre-send shape");
    CHECK(obs.list_copy, "AC11: a sender-side set-car! is invisible in the received list");
    CHECK(obs.vec_shape, "AC11: the received vector has the sender's pre-send shape");
    CHECK(obs.vec_copy, "AC11: a sender-side vector-set! is invisible in the received vector");
    CHECK(obs.hash_value, "AC11: the received hash has the sender's pre-send value");
    CHECK(obs.hash_copy, "AC11: a sender-side hash-set! is invisible in the received hash");
    CHECK(obs.hash_list_value, "AC11: the hash's list value survived the copy");
    delete sched;
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
    // Issue #4744 — immediate payloads (number / bool / void / float).
    ac8_immediate_payload_4744();
    ac9_undeliverable_payload_4744();
    ac10_source_4744();
    // Issue #4763 — structured payloads are deep-copied on send.
    ac11_deep_copy_4763();
    std::println("\n=== #2483 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_channel_rendezvous();
}
#endif
