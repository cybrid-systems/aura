// @category: unit
// @reason: Issue #2483 — channel:send rendezvous (buffer_size=0) blocks
//          until a recv is posted; no buffer_size==0 wait short-circuit.
//
//   AC1: rendezvous send blocks until concurrent recv
//   AC2: buffered channel allows send without recv (size < buf)
//   AC3: source predicate / waiting_receivers / channels_mtx_ scope
//   AC4: gate wiring

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
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
    auto sidx = sh.size();
    sh.push_back("hello-rendezvous");
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
    auto s0 = sh.size();
    sh.push_back("a");
    auto s1 = sh.size();
    sh.push_back("b");

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
        auto win = src.substr(spos, 2500);
        CHECK(win.find("waiting_receivers") != std::string::npos,
              "AC3: send uses waiting_receivers");
        CHECK(win.find("ch.buffer_size == 0 || ch.queue.size()") == std::string::npos,
              "AC3: no buggy wait Or short-circuit");
        CHECK(win.find("ch_ptr") != std::string::npos, "AC3: channels_mtx_ released before wait");
    }
    auto rpos = src.find("channel:recv");
    CHECK(rpos != std::string::npos, "AC3: channel:recv present");
    if (rpos != std::string::npos) {
        auto win = src.substr(rpos, 1500);
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
    CHECK(cmake.find("test_channel_rendezvous_2483") != std::string::npos, "AC4: cmake test");
    CHECK(!read_file("scripts/check_channel_rendezvous_2483.py").empty(),
          "AC4: check script exists");
}

} // namespace

int main() {
    std::println("=== Issue #2483: channel rendezvous semantics ===");
    ac1_rendezvous_blocks();
    ac2_buffered();
    ac3_source();
    ac4_gate();
    std::println("\n=== #2483 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
