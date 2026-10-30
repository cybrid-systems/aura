// @category: unit
// @reason: Issue #4747 (GAP 2) - a file-heap sweep started from one fiber
// body must not free a SIBLING fiber body's live IR frames on the SERVE path.
//
// Why this is a C++ test and not a tests/suite/*.aura row: in the CLI --load
// environment a fiber:spawn body attaches NO tree-walker segment (#4713
// attaches only from src/serve/fiber.cpp), so it walks in its worker thread's
// PLAIN segment and aura_tw_plain_walkers_other() counts it. Both destructive
// sweeps already stand down on that pre-existing #2738 count, before and after
// the #4747 repair, so a CLI row passes either way and cannot discriminate the
// rule. The rule bde9e33cd adds (aura_ir_active_drains_total) is reachable
// only for a body class the plain count does NOT cover: a serve-scheduled body
// with an attached fiber segment.
//
// This test therefore starts its bodies through aura::serve::Scheduler (the
// same resume path src/serve/fiber.cpp uses: aura_tw_roots_attach on resume,
// tenant scope + boundary audit on the fiber). Each body then owns a
// registered segment, aura_tw_plain_walkers_other() is 0 for it, and the only
// stand-down input left for the IR-body class is the drain count.
//
// Row shape. The workspace defines are installed through (set-code ...), which
// is what lowers and CACHES their IR; a plain eval of the define forms leaves
// tree-walker closures and no IR drain at all.
//
//   body A: t4747-hold builds a 20000-row pair list ONE ROW AT A TIME, with a
//           pure arithmetic spin between rows (no primitive call, so its IR
//           drain is never unwound), then folds the list with a bounded
//           traversal and returns the sum. The list is under construction for
//           the whole window on purpose: the file-mode reclaim only frees pair
//           slots minted inside its current mark cycle, so a list that was
//           finished before the sibling's first mark is not a candidate and a
//           collector that cannot mark A's frames would still look innocent.
//   body B: t4747-churn mints pairs in its own body; its allocation-triggered
//           sweeps are the collector under test.
//
// A's body is spawned first and B only after A's body has started, so A is
// inside its IR drain before B can reach its first sweep check. Without that
// handshake the row would also measure the check-then-act entry window every
// counter-based stand-down has, and would fail at HEAD in some runs.
//
// AC1: with B sweeping while A builds and holds, A's fold must return the
//      exact sum it built - 10/10 rounds. A collector that cannot certify A's
//      live IR frame set frees rows A already built, B reuses those slots, and
//      A's fold reads B's data (a wrong sum) or a spliced chain (sentinel).
// AC2: the control - the same body applied serially, no sibling - returns the
//      same sum, so an AC1 failure cannot be blamed on the row shape.
// AC3: the round only counts when both bodies finished inside the bounded wall
//      clock and their windows overlapped (a serial round is a FAIL, never a
//      skip).

#include "test_harness.hpp"

#include "serve/fiber.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <print>
#include <string>
#include <thread>

import std;
import aura.compiler.service;
import aura.compiler.value;

// Issue #4791: the drain-begin half of the #4768 sweep gate (runtime_ssot.cpp).
extern "C" std::uint64_t aura_gc_sweep_gate_body_hits_total() noexcept;
extern "C" std::uint64_t aura_gc_sweep_gate_body_waits_total() noexcept;
extern "C" void aura_gc_sweep_gate_set_drain_nowait(int on) noexcept;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::as_int;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_int;
using aura::serve::Scheduler;
using aura::test::g_failed;
using aura::test::g_passed;

// 2000 rows of (i, 2i, 'row i); the fold of car+cdr over every row is
// sum(i) + sum(2i) = 3 * 1999 * 2000 / 2.
constexpr std::int64_t kRows = 2000;
constexpr std::int64_t kExpectedSum = 5997000;
// Pure arithmetic iterations between two rows: 2000 * 7500 = 15M, the same
// hold window the CLI probe used (about 4 s of IR spin). The build therefore
// spans the whole window; only the row count shrank so the fold stays cheap.
constexpr std::int64_t kRowSpin = 7500;
// Sibling mints: the same ratio the CLI probe used, where every mint fell
// inside the hold window.
constexpr std::int64_t kChurnMints = 600000;
constexpr int kRounds = 10;
constexpr auto kRoundBudget = std::chrono::seconds(120);
constexpr auto kHoldStartBudget = std::chrono::seconds(5);

// Workspace defines, lowered to IR. The hold body and the sibling mint body
// are two DIFFERENT defines, so each owns its interpreter and its exec mutex
// and the two bodies can be inside IR at the same time.
constexpr const char* kDefines = R"SRC(
(define (t4747-mk i) (cons i (cons (* i 2) (list 'row i))))
(define (t4747-sum l n)
  (let loop ((xs l) (k 0) (s 0))
    (cond ((> k n) -4)
          ((null? xs) (if (= k n) s -5))
          ((not (pair? xs)) -1)
          ((not (pair? (car xs))) -2)
          (else
            (let ((e (car xs)))
              (if (and (number? (car e))
                       (pair? (cdr e))
                       (number? (car (cdr e))))
                  (loop (cdr xs) (+ k 1) (+ s (+ (car e) (car (cdr e)))))
                  -3))))))
(define (t4747-hold n step)
  (let loop ((i 0) (k 0) (acc (list)))
    (cond ((>= i n) (t4747-sum acc n))
          ((>= k step) (loop (+ i 1) 0 (cons (t4747-mk i) acc)))
          (else (loop i (+ k 1) acc)))))
(define (t4747-churn m)
  (let loop ((i 0) (acc (list)))
    (if (= i m) (length acc) (loop (+ i 1) (cons (cons i i) acc)))))
)SRC";

// Install the defines the way the CLI row and the serve workspace do: the
// (set-code ...) primitive lowers and CACHES IR for every top-level define, so
// a later call from a body dispatches into that define's own IR interpreter
// (an IR drain). A plain cs.eval of the define forms only binds tree-walker
// closures.
static bool install_defines(CompilerService& cs) {
    std::string one_line;
    for (const char* p = kDefines; *p != 0; ++p)
        one_line.push_back(*p == '\n' ? ' ' : *p);
    const auto program = std::string("(set-code \"") + one_line + "\") (eval-current)";
    return cs.eval(program).has_value();
}

// One round through the serve scheduler: A builds+holds, B mints, both on
// worker threads with their own attached tree-walker segments.
struct Round {
    std::int64_t held = -1;
    std::int64_t minted = -1;
    bool a_finished = false;
    bool b_finished = false;
    bool overlapped = false;
};

static Round run_sibling_round(std::int64_t rows, std::int64_t step, std::int64_t mints,
                               bool simultaneous = false) {
    Round out;
    CompilerService cs;
    auto& ev = cs.evaluator();

    if (!install_defines(cs)) {
        std::println(std::cerr, "  defines failed to install");
        return out;
    }
    const auto hold_src = std::string("(lambda () (t4747-hold ") + std::to_string(rows) + " " +
                          std::to_string(step) + "))";
    const auto churn_src = std::string("(lambda () (t4747-churn ") + std::to_string(mints) + "))";
    const auto hold_v = cs.eval(hold_src);
    const auto churn_v = cs.eval(churn_src);
    if (!hold_v || !is_closure(*hold_v) || !churn_v || !is_closure(*churn_v)) {
        std::println(std::cerr, "  body closures did not resolve");
        return out;
    }
    const auto hold_cid = as_closure_id(*hold_v);
    const auto churn_cid = as_closure_id(*churn_v);

    // 0 = pending, 1 = ok, 2 = failed.
    std::atomic<int> a_state{0};
    std::atomic<int> b_state{0};
    std::atomic<std::int64_t> a_val{-1};
    std::atomic<std::int64_t> b_val{-1};
    std::atomic<std::int64_t> a_beg{0};
    std::atomic<std::int64_t> a_end{0};
    std::atomic<std::int64_t> b_beg{0};
    std::atomic<std::int64_t> b_end{0};

    Scheduler sched(2);
    // Issue #4726: this test owns the scheduler; the idle auto-stop would
    // stop the workers under a round that is allowed to run long.
    sched.set_idle_auto_stop(false);
    std::thread io([&sched] { sched.run(); });

    const auto origin = std::chrono::steady_clock::now();
    auto stamp = [origin] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - origin)
            .count();
    };

    // The body closures are tree-walker temporaries: nothing in the env
    // references them, and A's own mint sweeps reach the evaluator's file-heap
    // reclaim while B is still pending. Root them for the round exactly the way
    // the aura fiber:spawn path does (#4697 / #4698), or the sibling body can
    // be collected before it is applied and the round proves nothing.
    aura_fiber_root_pending_body(&ev, static_cast<std::uint64_t>(hold_cid), 1);
    aura_fiber_root_pending_body(&ev, static_cast<std::uint64_t>(churn_cid), 2);

    sched.spawn([&] {
        a_beg.store(stamp());
        auto r = ev.apply_closure(hold_cid, {});
        a_end.store(stamp());
        if (r && is_int(*r)) {
            a_val.store(as_int(*r));
            a_state.store(as_int(*r) == kExpectedSum ? 1 : 2);
        } else {
            a_state.store(2);
        }
    });

    // Spawn the sibling only after A's body has started. A reaches its define
    // (and therefore its IR drain) within microseconds, while the sibling
    // still has to be scheduled, enter its own define and mint its way to the
    // first sweep -- so A is inside the drain before any sibling sweep checks.
    // Without this, the row would also measure the check-then-act entry window
    // every counter-based stand-down has.
    // Issue #4791: the simultaneous-spawn row skips this handshake on purpose;
    // it is the row that measures that entry window.
    if (!simultaneous) {
        const auto start_deadline = std::chrono::steady_clock::now() + kHoldStartBudget;
        while (a_beg.load() == 0 && std::chrono::steady_clock::now() < start_deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    sched.spawn([&] {
        b_beg.store(stamp());
        auto r = ev.apply_closure(churn_cid, {});
        b_end.store(stamp());
        if (r && is_int(*r)) {
            b_val.store(as_int(*r));
            b_state.store(1);
        } else {
            b_state.store(2);
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + kRoundBudget;
    while ((a_state.load() == 0 || b_state.load() == 0) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    sched.stop();
    io.join();
    aura_fiber_release_pending_body(&ev, 1);
    aura_fiber_release_pending_body(&ev, 2);

    out.a_finished = a_state.load() != 0;
    out.b_finished = b_state.load() != 0;
    out.held = a_val.load();
    out.minted = b_val.load();
    out.overlapped = a_beg.load() < b_end.load() && b_beg.load() < a_end.load();
    return out;
}

// The control: the same hold body applied on this thread, no sibling body, so
// no allocation-triggered sweep from a second IR drain can run.
static std::int64_t run_control(std::int64_t rows, std::int64_t step) {
    CompilerService cs;
    auto& ev = cs.evaluator();
    if (!install_defines(cs))
        return -1;
    const auto hold_src = std::string("(lambda () (t4747-hold ") + std::to_string(rows) + " " +
                          std::to_string(step) + "))";
    const auto hold_v = cs.eval(hold_src);
    if (!hold_v || !is_closure(*hold_v))
        return -1;
    auto r = ev.apply_closure(as_closure_id(*hold_v), {});
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void ac2_control() {
    std::println("\n--- #4747 AC2: serial control (no sibling body) ---");
    const auto sum = run_control(kRows, kRowSpin);
    CHECK(sum == kExpectedSum,
          std::format("AC2: serial build+fold returns {} (got {})", kExpectedSum, sum));
}

void ac1_sibling_hold() {
    std::println("\n--- #4747 AC1: sibling mint sweep vs held IR frame set (serve bodies) ---");
    int held_ok = 0;
    int bad_sum = 0;
    int unfinished = 0;
    int serialised = 0;
    for (int i = 0; i < kRounds; ++i) {
        const auto r = run_sibling_round(kRows, kRowSpin, kChurnMints);
        std::println("  round {}: held={} minted={} a_done={} b_done={} overlapped={}", i, r.held,
                     r.minted, r.a_finished, r.b_finished, r.overlapped);
        if (!r.a_finished || !r.b_finished || r.minted != kChurnMints) {
            ++unfinished;
            continue;
        }
        if (!r.overlapped)
            ++serialised;
        if (r.held == kExpectedSum)
            ++held_ok;
        else
            ++bad_sum;
    }
    CHECK(
        unfinished == 0,
        std::format("AC1: every round finished inside the wall bound ({} unfinished)", unfinished));
    CHECK(serialised == 0,
          std::format("AC1: the sibling's mint window overlapped the hold in {}/{} rounds "
                      "({} ran serially and would not exercise the rule)",
                      kRounds - serialised, kRounds, serialised));
    CHECK(bad_sum == 0, std::format("AC1: the held list survived the sibling mint sweep in "
                                    "{}/{} rounds ({} corrupted)",
                                    held_ok, kRounds, bad_sum));
    CHECK(held_ok == kRounds,
          std::format("AC1: {}/{} rounds returned the exact held sum", held_ok, kRounds));
}

// Issue #4791: SIMULTANEOUS spawn - both bodies are queued back to back, so
// either body's first IR drain can begin while the other body's sweep already
// holds the gate and has read the drain count. Before #4791 the drain begin
// only published the count (check-then-act from the sweep's side) and the late
// body minted straight under the release: ~1 round in 10 corrupted in 2 of 5
// runs. Now a drain begin that finds the gate held waits the sweep out.
//
// The row uses a shorter hold spin than AC1: the window is at body START, so a
// long hold only makes each round slower without hitting it more often.
// AURA_4791_ROUNDS overrides the round count (acceptance ran 100).
constexpr std::int64_t kSimRowSpin = 1500;
constexpr std::int64_t kSimChurnMints = 200000;

static int sim_rounds() {
    const char* e = std::getenv("AURA_4791_ROUNDS");
    if (e != nullptr && *e != 0) {
        const int n = std::atoi(e);
        if (n > 0)
            return n;
    }
    return 20;
}

struct SimTally {
    int ok = 0;
    int bad = 0;
    int unfinished = 0;
    int serialised = 0;
    std::uint64_t hits = 0;
    std::uint64_t waits = 0;
};

static SimTally run_sim_arm(int rounds, bool nowait) {
    SimTally t;
    aura_gc_sweep_gate_set_drain_nowait(nowait ? 1 : 0);
    const auto hits0 = aura_gc_sweep_gate_body_hits_total();
    const auto waits0 = aura_gc_sweep_gate_body_waits_total();
    for (int i = 0; i < rounds; ++i) {
        const auto r = run_sibling_round(kRows, kSimRowSpin, kSimChurnMints, /*simultaneous=*/true);
        if (!r.a_finished || !r.b_finished || r.minted != kSimChurnMints) {
            ++t.unfinished;
            std::println("  {} round {}: unfinished held={} minted={}", nowait ? "nowait" : "gate",
                         i, r.held, r.minted);
            continue;
        }
        if (!r.overlapped)
            ++t.serialised;
        if (r.held == kExpectedSum) {
            ++t.ok;
        } else {
            ++t.bad;
            std::println("  {} round {}: held={} (corrupted)", nowait ? "nowait" : "gate", i,
                         r.held);
        }
    }
    t.hits = aura_gc_sweep_gate_body_hits_total() - hits0;
    t.waits = aura_gc_sweep_gate_body_waits_total() - waits0;
    aura_gc_sweep_gate_set_drain_nowait(0);
    return t;
}

// AC4: the fixed gate - zero corruption over the simultaneous rounds, and the
//      instrumented window count shows the row actually reached the window
//      (hits > 0 across the run) and that every hit waited.
// AC5 (measurement, informational unless AURA_4791_CONTROL=1): the same rows
//      with the drain-begin wait disarmed (the pre-#4791 behaviour) - prints
//      how often the window was hit and how many rounds corrupted.
void ac4_simultaneous_spawn() {
    std::println("\n--- #4791 AC4: simultaneous spawn, drain begin participates in the gate ---");
    const int rounds = sim_rounds();
    const auto g = run_sim_arm(rounds, /*nowait=*/false);
    std::println("  gate arm: {} rounds ok={} corrupted={} unfinished={} serial={} "
                 "window-hits={} waited={}",
                 rounds, g.ok, g.bad, g.unfinished, g.serialised, g.hits, g.waits);
    CHECK(g.unfinished == 0,
          std::format("AC4: every simultaneous round finished ({} unfinished)", g.unfinished));
    CHECK(g.bad == 0,
          std::format("AC4: zero corrupted simultaneous-spawn rounds ({}/{} ok)", g.ok, rounds));
    CHECK(g.waits == g.hits,
          std::format("AC4: every window hit waited the sweep out ({} hits, {} waits)", g.hits,
                      g.waits));
    const char* ctl = std::getenv("AURA_4791_CONTROL");
    if (ctl != nullptr && *ctl == '1') {
        std::println("\n--- #4791 AC5: control arm (drain begin does not wait; pre-fix) ---");
        const auto c = run_sim_arm(rounds, /*nowait=*/true);
        std::println("  control arm: {} rounds ok={} corrupted={} unfinished={} serial={} "
                     "window-hits={} waited={}",
                     rounds, c.ok, c.bad, c.unfinished, c.serialised, c.hits, c.waits);
        CHECK(c.waits == 0, "AC5: the control arm never waits a drain begin");
    }
}

} // namespace

int main() {
    std::println("=== Issue #4747 GAP 2: serve-body sibling IR collector ===");
    // Issue #3586: Scheduler::run refuses an unarmed multi-worker pool. This
    // row needs two workers -- the sibling sweep has to run on another thread
    // than the hold -- so take the same documented escape the serve chaos gate
    // test uses (AURA_SANDBOX=off) and put the process face back after.
    const char* prev_sandbox = std::getenv("AURA_SANDBOX");
    const std::string saved_sandbox = prev_sandbox != nullptr ? prev_sandbox : "";
    ::setenv("AURA_SANDBOX", "off", 1);
    ac2_control();
    ac1_sibling_hold();
    ac4_simultaneous_spawn();
    if (prev_sandbox != nullptr)
        ::setenv("AURA_SANDBOX", saved_sandbox.c_str(), 1);
    else
        ::unsetenv("AURA_SANDBOX");
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
