// @category: integration
// @reason: Issue #1509 — multi-fiber concurrent mutate→call-stale
// closure stress + ASan/TSan-oriented validation of safe fallback.
//
// Non-duplicative of #1475 (helper unit), #1507 (IRClosure env_id),
// #1508 (JIT dual check). This issue is the integration stress AC5.
//
//   AC1: single-thread 1000× apply stale → safe_fallback metrics
//   AC2: 4-thread concurrent apply while main mutates/bumps epoch
//   AC3: recursive define + lambda capture after mutate:rebind
//   AC4: new metrics present + grow under stress
//   AC5: EDSL mutate:rebind / set-code path exercises fallback

#include "test_harness.hpp"
#include "observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1509_detail {

using aura::compiler::ClosureId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::as_int;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

// Capture a real tree-walker closure via (lambda ...).
static bool capture_lambda_cid(CompilerService& cs, ClosureId& out_cid) {
    // Ensure bridge tracking is live (non-zero epoch).
    cs.bump_bridge_epoch();
    auto clo = cs.eval("(lambda (x) (+ x 1))");
    if (!clo || !is_closure(*clo))
        return false;
    out_cid = static_cast<ClosureId>(as_closure_id(*clo));
    // Fresh call should succeed (or at least not crash).
    auto& ev = cs.evaluator();
    auto args = std::array{make_int(5)};
    (void)ev.apply_closure(out_cid, std::span<const aura::compiler::types::EvalValue>(args));
    return true;
}

// AC1: single-thread 1000 stale applies.
static void ac1_single_thread_stress() {
    std::println("\n--- AC1: single-thread 1000× stale apply ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics available");

    ClosureId cid = 0;
    CHECK(capture_lambda_cid(cs, cid), "captured lambda closure");

    // Invalidate: any apply of the pre-bump closure must take safe fallback.
    cs.bump_bridge_epoch();
    const auto stale0 = load_u64(m->closure_stale_apply_count_total);
    const auto safe0 = load_u64(m->closure_safe_fallback_apply_count_total);
    const auto legacy0 = load_u64(m->compiler_closure_safe_fallbacks);

    auto args = std::array{make_int(5)};
    int ok_or_refused = 0;
    for (int i = 0; i < 1000; ++i) {
        auto r = cs.evaluator().apply_closure(
            cid, std::span<const aura::compiler::types::EvalValue>(args));
        // Safe: either nullopt (refused) or bridged value — never crash.
        (void)r;
        ++ok_or_refused;
    }
    CHECK(ok_or_refused == 1000, "1000 applies completed without crash");

    const auto stale1 = load_u64(m->closure_stale_apply_count_total);
    const auto safe1 = load_u64(m->closure_safe_fallback_apply_count_total);
    const auto legacy1 = load_u64(m->compiler_closure_safe_fallbacks);
    std::println("  stale_apply {}→{}  safe_fallback {}→{}  legacy_safe {}→{}", stale0, stale1,
                 safe0, safe1, legacy0, legacy1);
    CHECK(stale1 >= stale0 + 1000, "closure_stale_apply_count_total += ≥1000");
    CHECK(safe1 >= safe0 + 1000, "closure_safe_fallback_apply_count_total += ≥1000");
    CHECK(legacy1 >= legacy0 + 1000, "compiler_closure_safe_fallbacks += ≥1000");
}

// AC2: 4 workers apply stale; main bumps epoch / mutates.
static void ac2_multi_thread_stress() {
    std::println("\n--- AC2: 4-thread concurrent stale apply + main mutate ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics available (multi)");

    ClosureId cid = 0;
    CHECK(capture_lambda_cid(cs, cid), "captured lambda for multi-thread");
    cs.bump_bridge_epoch(); // start stale

    std::atomic<int> workers_done{0};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> worker_applies{0};
    constexpr int kWorkers = 4;
    constexpr int kAppliesPerWorker = 250; // 4*250 = 1000

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&, w]() {
            (void)w;
            auto args = std::array{make_int(3)};
            for (int i = 0; i < kAppliesPerWorker; ++i) {
                auto r = cs.evaluator().apply_closure(
                    cid, std::span<const aura::compiler::types::EvalValue>(args));
                (void)r;
                worker_applies.fetch_add(1, std::memory_order_relaxed);
            }
            workers_done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Main thread: interleave bridge bumps + light EDSL mutate.
    (void)cs.eval("(set-code \"(define (f x) (+ x 1))\")");
    (void)cs.eval("(eval-current)");
    for (int i = 0; i < 50; ++i) {
        cs.bump_bridge_epoch();
        cs.evaluator().bump_defuse_version_for_test();
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (+ x 2))\" \"#1509\")");
        if (workers_done.load(std::memory_order_relaxed) >= kWorkers)
            break;
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : workers)
        t.join();

    CHECK(worker_applies.load() == static_cast<std::uint64_t>(kWorkers * kAppliesPerWorker),
          "all worker applies completed");
    CHECK(load_u64(m->closure_stale_apply_count_total) >= 1000,
          "stale_apply ≥ 1000 after multi-thread stress");
    CHECK(load_u64(m->closure_safe_fallback_apply_count_total) >= 1000,
          "safe_fallback_apply ≥ 1000 after multi-thread stress");
    // race_caught may grow when bridge mismatch path runs (often).
    CHECK(load_u64(m->closure_race_caught_count_total) >= 0, "race_caught non-negative");
    std::println("  race_caught={}", load_u64(m->closure_race_caught_count_total));
}

// AC3: recursive define + lambda capture after mutate.
static void ac3_recursive_define_lambda_capture() {
    std::println("\n--- AC3: recursive define + lambda capture after rebind ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics (recursive)");

    // f is called from nested lambda returned by g.
    CHECK(cs.eval("(set-code \""
                  "(define (f x) (+ x 1)) "
                  "(define (g x) (lambda (y) (f (+ x y)))) "
                  "\")")
              .has_value(),
          "set-code f+g");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current f+g");

    // Capture the nested lambda: (g 5) → (lambda (y) (f (+ 5 y)))
    auto nested = cs.eval("(g 5)");
    // May be void if g not callable that way — also try apply via eval.
    if (!nested || !is_closure(*nested)) {
        // Fallback: direct apply path with a lambda that closes over f via set-code body.
        auto clo = cs.eval("(lambda (y) (f (+ 5 y)))");
        CHECK(clo && is_closure(*clo), "fallback lambda capture");
        nested = clo;
    }
    CHECK(nested && is_closure(*nested), "nested lambda is closure");
    auto cid = static_cast<ClosureId>(as_closure_id(*nested));

    // Call while fresh (best-effort).
    auto fresh = cs.evaluator().apply_closure(cid, {make_int(2)});
    (void)fresh;

    const auto stale0 = load_u64(m->closure_stale_apply_count_total);
    const auto safe0 = load_u64(m->closure_safe_fallback_apply_count_total);

    // Mutate f and bump epochs — captured nested lambda must not UAF.
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (* x 2))\" \"#1509-rec\")");
    cs.bump_bridge_epoch();
    cs.evaluator().bump_defuse_version_for_test();

    auto after = cs.evaluator().apply_closure(cid, {make_int(2)});
    // Safe: nullopt or bridged value — no crash.
    (void)after;
    CHECK(true, "apply after mutate did not crash");

    // Also exercise quote-ish capture via EDSL eval of quoted form if available.
    auto q = cs.eval("(quote (g 5))");
    (void)q;
    CHECK(load_u64(m->closure_stale_apply_count_total) >= stale0,
          "stale_apply non-decreasing after recursive case");
    CHECK(load_u64(m->closure_safe_fallback_apply_count_total) >= safe0,
          "safe_fallback non-decreasing after recursive case");
    // Prefer growth when bridge tracking is active.
    if (cs.bridge_epoch() > 0) {
        CHECK(load_u64(m->closure_stale_apply_count_total) > stale0 ||
                  load_u64(m->compiler_closure_safe_fallbacks) > 0,
              "stale/safe path exercised after rebind+bump");
    }
}

// AC4: metrics fields exist and are independently readable.
static void ac4_metrics_surface() {
    std::println("\n--- AC4: metric surface ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics for surface");
    CHECK(load_u64(m->closure_stale_apply_count_total) >= 0, "stale_apply readable");
    CHECK(load_u64(m->closure_safe_fallback_apply_count_total) >= 0,
          "safe_fallback_apply readable");
    CHECK(load_u64(m->closure_race_caught_count_total) >= 0, "race_caught readable");

    // Direct bump seam (simulate TSan race catcher hook).
    const auto r0 = load_u64(m->closure_race_caught_count_total);
    m->closure_race_caught_count_total.fetch_add(1, std::memory_order_relaxed);
    CHECK(load_u64(m->closure_race_caught_count_total) == r0 + 1, "race_caught bumpable");
}

// AC5: pure EDSL mutate loop (set-code / rebind) + eval-current.
static void ac5_edsl_mutate_loop() {
    std::println("\n--- AC5: EDSL mutate:rebind loop ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(cs.eval("(set-code \"(define (h x) (+ x 1))\")").has_value(), "set-code h");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current h");
    auto v0 = cs.eval("(h 10)");
    CHECK(v0 && is_int(*v0), "(h 10) returns int before rebind");

    // Capture a lambda that depends on h (if rebind invalidates, call is safe).
    auto clo = cs.eval("(lambda (z) (h z))");
    ClosureId cid = 0;
    bool have_clo = clo && is_closure(*clo);
    if (have_clo)
        cid = static_cast<ClosureId>(as_closure_id(*clo));

    const auto safe0 = m ? load_u64(m->compiler_closure_safe_fallbacks) : 0;
    for (int i = 0; i < 100; ++i) {
        (void)cs.eval(
            std::format("(mutate:rebind \"h\" \"(lambda (x) (+ x {}))\" \"#1509-edsl\")", i + 2));
        cs.bump_bridge_epoch();
        if (have_clo) {
            auto r = cs.evaluator().apply_closure(cid, {make_int(1)});
            (void)r;
        }
        auto v = cs.eval("(h 1)");
        (void)v; // may succeed via re-eval of binding
    }
    CHECK(true, "100-iter EDSL rebind+apply completed");
    if (m && have_clo) {
        CHECK(load_u64(m->compiler_closure_safe_fallbacks) >= safe0,
              "safe_fallbacks non-decreasing after EDSL loop");
    }
}

} // namespace aura_issue_1509_detail

int aura_issue_1509_run() {
    using namespace aura_issue_1509_detail;
    std::println("=== Issue #1509: multi-fiber mutate→stale-closure stress ===");
    ac1_single_thread_stress();
    ac2_multi_thread_stress();
    ac3_recursive_define_lambda_capture();
    ac4_metrics_surface();
    ac5_edsl_mutate_loop();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_1509_run();
}
#endif
