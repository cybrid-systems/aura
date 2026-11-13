// @category: unit
// @reason: Issue #5274 — a CLI thread-backend body parked in a channel wait
// must not freeze the file-heap sweeps for the lifetime of the wait.
//
// Why this is a C++ test: the body class under test is the fiber:spawn
// FALLBACK (no serve scheduler in this binary), which attaches NO tree-walker
// segment — its live frames sit in the worker thread's PLAIN segment. Before
// this repair both stand-down counts (aura_tw_plain_walkers_other and
// aura_fiber_bodies_running_other) saw that body for its whole lifetime, so a
// body blocked in channel:recv forever froze every destructive sweep: a
// long-lived --serve-async game grew to ~580-850 MB RSS and each outermost
// mutate paid a 0.4-1.5 s linear audit.
//
// The counters are read through their C linkage directly (the test binary
// links the evaluator TUs); the query:tw-plain-parks surface is the same
// number for a live game.
//
//   AC1: while the body is blocked, the park bracket engaged
//        (aura_tw_plain_park_total > 0), the segment is published markable
//        (aura_tw_plain_parked_now >= 1) and BOTH stand-down counts read 0 —
//        the certification, not a blind uncount.
//   AC2: a tree-walker sweep from the MAIN thread then runs while the body is
//        still parked (aura_tw_registry_reclaim_total grows). Pre-fix that
//        counter is frozen: the sweep stands down on the parked body. This is
//        the discriminating check.
//   AC3: the parked body is not corrupted: after those sweeps a message sent
//        on the channel is received intact and fiber:join returns it. A
//        collector that ran without certifying the segment frees the body's
//        env frames / pair cells and the join body reads Void or loops on
//        spliced cells (the #4747 face).
//   AC4: the bracket is transparent when nothing blocks — a pure loop mints
//        no park, so the common path pays nothing.

#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <print>
#include <string>
#include <thread>

import std;
import aura.compiler.service;
import aura.compiler.value;

// Issue #5274: certification + sweep counters (evaluator_eval_flat.cpp /
// runtime_ssot.cpp, C linkage like the rest of the walker/GC bridge).
extern "C" std::uint64_t aura_tw_plain_park_total() noexcept;
extern "C" int aura_tw_plain_parked_now() noexcept;
extern "C" std::uint64_t aura_tw_registry_reclaim_total() noexcept;
extern "C" int aura_tw_plain_walkers_other() noexcept;
extern "C" int aura_fiber_bodies_running_other() noexcept;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

constexpr std::int64_t kChurnConses = 400000;
constexpr auto kParkBudget = std::chrono::seconds(10);

static std::int64_t eval_int(CompilerService& cs, const std::string& src, const char* what) {
    auto r = cs.eval(src);
    if (!r || !is_int(*r)) {
        std::println(std::cerr, "  {} did not return an int: {}", what, src);
        ++g_failed;
        return -1;
    }
    return as_int(*r);
}

// The churn is a tree-walked let loop (no define / IR cache), so its
// allocations reach Evaluator::maybe_reclaim_tw_unreachable — the sweep whose
// stand-down decision is under test.
static void churn(CompilerService& cs, std::int64_t n) {
    const auto src = std::string("(let loop ((i 0) (acc (list))) (if (= i ") + std::to_string(n) +
                     ") (length acc) (loop (+ i 1) (cons i acc))))";
    auto r = cs.eval(src);
    CHECK(r && is_int(*r) && as_int(*r) == n, "churn returned the consed count");
}

void ac1_park_and_reclaim() {
    std::println("── AC1/AC2: a blocked body certifies its plain segment ──");
    CompilerService cs;

    CHECK(cs.eval("(define t5274-ch (channel:create 1))").has_value(), "AC1: channel created");
    CHECK(cs.eval("(define t5274-worker (lambda () (channel:recv t5274-ch)))").has_value(),
          "AC1: worker closure defined");
    const auto fid = eval_int(cs, "(fiber:spawn t5274-worker)", "fiber:spawn");
    CHECK(fid > 0, "AC1: body spawned on the thread backend");

    // The body reaches channel:recv (and its host wait) within microseconds;
    // poll for the published park rather than sleeping a fixed span.
    std::int64_t parked_now = 0;
    int walkers_other = -1;
    int bodies_other = -1;
    const auto deadline = std::chrono::steady_clock::now() + kParkBudget;
    while (std::chrono::steady_clock::now() < deadline) {
        parked_now = aura_tw_plain_parked_now();
        walkers_other = aura_tw_plain_walkers_other();
        bodies_other = aura_fiber_bodies_running_other();
        if (parked_now >= 1 && walkers_other == 0 && bodies_other == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(aura_tw_plain_park_total() > 0, "AC1: the park bracket engaged");
    CHECK(parked_now >= 1, "AC1: the blocked body's plain segment is markable");
    CHECK(walkers_other == 0, "AC1: no uncertified plain walker remains");
    CHECK(bodies_other == 0, "AC1: no uncertified body remains");

    // AC2: the sweep runs while the body is still parked.
    const auto before = aura_tw_registry_reclaim_total();
    churn(cs, kChurnConses);
    const auto after = aura_tw_registry_reclaim_total();
    CHECK(after > before, "AC2: a sweep ran with the body parked (reclaim count grew)");
    CHECK(aura_tw_plain_parked_now() >= 1, "AC2: the body stayed parked across the sweep");

    // AC3: the parked body's frames survived those sweeps.
    CHECK(cs.eval("(channel:send t5274-ch 424242)").has_value(), "AC3: message sent");
    const auto joined = eval_int(cs, "(fiber:join " + std::to_string(fid) + ")", "fiber:join");
    CHECK(joined == 424242, "AC3: the parked body resumed and returned the message intact");
}

// AC4: nothing blocks, nothing is parked.
void ac4_no_park_without_wait() {
    std::println("── AC4: no park for a body that does not wait ──");
    CompilerService cs;
    const auto before = aura_tw_plain_park_total();
    const auto v = eval_int(cs, "(let loop ((i 0) (s 0)) (if (= i 1000) s (loop (+ i 1) (+ s i))))",
                            "pure loop");
    CHECK(v == 499500, "AC4: pure loop value exact");
    CHECK(aura_tw_plain_park_total() == before, "AC4: a non-blocking eval publishes no park");
}

} // namespace

int run_test_tw_plain_park_reclaim() {
    std::println("=== Issue #5274: a parked body no longer freezes the sweeps ===");
    ac1_park_and_reclaim();
    ac4_no_park_without_wait();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_tw_plain_park_reclaim();
}
#endif
