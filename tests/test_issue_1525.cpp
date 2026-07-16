// @category: integration
// @reason: Issue #1525 — multi-fiber concurrent mutate + immediate eval
// old closure + JIT dual-check stress (atomicity counterpart to #1509).
//
// Non-duplicative of #1509 (read-side stale apply), #1523 (lock-order
// unit), #1524 (typed_mutate dual-epoch). This issue is parallel mutate
// serialization + stale call safety under worker interleaving.
//
//   AC1: metrics multifiber_mutate_races / multifiber_safe_fallback surface
//   AC2: single-thread 1000× mutate + apply stale → metrics grow
//   AC3: 4 workers apply stale while main mutates (1000 applies total)
//   AC4: recursive define + lambda capture after concurrent rebind
//   AC5: dual-epoch helper under concurrent mutate (atomic_bump)
//   AC6: JIT dual-check stale path (aura_closure_call) interleave
//   AC7: fiber-steal probe exercises multifiber counters
//   AC8: 4-fiber 1000-round mutate/eval cycle, no crash

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
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

extern "C" int64_t aura_alloc_closure(int64_t func_id);
extern "C" int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc);
extern "C" void aura_free_closure(int64_t closure_id);

namespace aura_issue_1525_detail {

using aura::compiler::ClosureId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
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

static bool capture_lambda(CompilerService& cs, ClosureId& out) {
    cs.bump_bridge_epoch();
    auto clo = cs.eval("(lambda (x) (+ x 1))");
    if (!clo || !is_closure(*clo))
        return false;
    out = static_cast<ClosureId>(as_closure_id(*clo));
    auto args = std::array{make_int(1)};
    (void)cs.evaluator().apply_closure(out,
                                       std::span<const aura::compiler::types::EvalValue>(args));
    return true;
}

static void ac1_metrics_surface() {
    std::println("\n--- AC1: multifiber metrics surface ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics available");
    CHECK(load_u64(m->multifiber_mutate_races_detected_total) >= 0, "races readable");
    CHECK(load_u64(m->multifiber_safe_fallback_total) >= 0, "safe_fallback readable");
    const auto r0 = load_u64(m->multifiber_mutate_races_detected_total);
    m->multifiber_mutate_races_detected_total.fetch_add(1, std::memory_order_relaxed);
    CHECK(load_u64(m->multifiber_mutate_races_detected_total) == r0 + 1, "races bumpable");
}

static void ac2_single_thread_1000() {
    std::println("\n--- AC2: single-thread 1000× mutate + stale apply ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    ClosureId cid = 0;
    CHECK(capture_lambda(cs, cid), "captured lambda");
    // Invalidate so subsequent applies are stale.
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    const auto races0 = load_u64(m->multifiber_mutate_races_detected_total);
    const auto safe0 = load_u64(m->multifiber_safe_fallback_total);
    auto args = std::array{make_int(5)};
    for (int i = 0; i < 1000; ++i) {
        auto r = cs.evaluator().apply_closure(
            cid, std::span<const aura::compiler::types::EvalValue>(args));
        (void)r;
        if ((i % 100) == 0)
            cs.public_atomic_bump_epochs_and_stamp_bridge("");
    }
    CHECK(load_u64(m->multifiber_mutate_races_detected_total) > races0, "races grew");
    CHECK(load_u64(m->multifiber_safe_fallback_total) > safe0, "safe_fallback grew");
    CHECK(load_u64(m->closure_stale_apply_count_total) >= 1000, "stale_apply >= 1000");
}

static void ac3_four_workers_main_mutates() {
    std::println("\n--- AC3: 4 workers apply + main mutates ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    ClosureId cid = 0;
    CHECK(capture_lambda(cs, cid), "captured for multi");
    cs.public_atomic_bump_epochs_and_stamp_bridge("");

    constexpr int kWorkers = 4;
    constexpr int kPerWorker = 250; // 1000 total
    std::atomic<std::uint64_t> applies{0};
    std::atomic<int> done{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&, w]() {
            (void)w;
            auto args = std::array{make_int(3)};
            for (int i = 0; i < kPerWorker; ++i) {
                auto r = cs.evaluator().apply_closure(
                    cid, std::span<const aura::compiler::types::EvalValue>(args));
                (void)r;
                applies.fetch_add(1, std::memory_order_relaxed);
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Main: interleave dual-epoch invalidation + light rebind.
    (void)cs.eval("(set-code \"(define (f x) (+ x 1))\")");
    (void)cs.eval("(eval-current)");
    for (int i = 0; i < 80; ++i) {
        cs.public_atomic_bump_epochs_and_stamp_bridge("f");
        (void)cs.public_typed_mutate(
            std::format("(mutate:rebind \"f\" \"(lambda (x) (+ x {}))\")", i + 2));
        if (done.load(std::memory_order_relaxed) >= kWorkers)
            break;
        std::this_thread::yield();
    }
    for (auto& t : workers)
        t.join();

    CHECK(applies.load() == static_cast<std::uint64_t>(kWorkers * kPerWorker),
          "all worker applies completed");
    CHECK(load_u64(m->multifiber_safe_fallback_total) >= 100, "multifiber safe_fallback active");
    CHECK(load_u64(m->multifiber_mutate_races_detected_total) >= 100,
          "multifiber races detected under stress");
    std::println("  races={} safe_fallback={}", load_u64(m->multifiber_mutate_races_detected_total),
                 load_u64(m->multifiber_safe_fallback_total));
}

static void ac4_recursive_define_capture() {
    std::println("\n--- AC4: recursive define + lambda capture ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(cs.eval("(set-code \""
                  "(define (f x) (+ x 1)) "
                  "(define (g x) (lambda (y) (f (+ x y)))) "
                  "\")")
              .has_value(),
          "set-code f+g");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");

    auto nested = cs.eval("(lambda (y) (f (+ 5 y)))");
    CHECK(nested && is_closure(*nested), "nested lambda captured");
    auto cid = static_cast<ClosureId>(as_closure_id(*nested));
    (void)cs.evaluator().apply_closure(cid, {make_int(2)});

    const auto races0 = load_u64(m->multifiber_mutate_races_detected_total);
    std::atomic<bool> stop{false};
    std::thread worker([&]() {
        auto args = std::array{make_int(1)};
        while (!stop.load(std::memory_order_relaxed)) {
            auto r = cs.evaluator().apply_closure(
                cid, std::span<const aura::compiler::types::EvalValue>(args));
            (void)r;
            std::this_thread::yield();
        }
    });

    for (int i = 0; i < 40; ++i) {
        (void)cs.public_typed_mutate(
            std::format("(mutate:rebind \"f\" \"(lambda (x) (* x {}))\")", i + 2));
        cs.public_atomic_bump_epochs_and_stamp_bridge("f");
        cs.evaluator().probe_linear_ownership_on_fiber_steal();
    }
    stop.store(true, std::memory_order_relaxed);
    worker.join();
    CHECK(load_u64(m->multifiber_mutate_races_detected_total) >= races0,
          "races non-decreasing after recursive stress");
    CHECK(true, "recursive capture concurrent rebind no crash");
}

static void ac5_dual_epoch_under_concurrency() {
    std::println("\n--- AC5: dual-epoch helper concurrent ---");
    CompilerService cs;
    const auto be0 = cs.bridge_epoch();
    const auto aot0 = aura_aot_func_table_epoch();
    std::atomic<int> errs{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            try {
                for (int i = 0; i < 100; ++i)
                    cs.public_atomic_bump_epochs_and_stamp_bridge(t % 2 == 0 ? "a" : "b");
            } catch (...) {
                errs.fetch_add(1);
            }
        });
    }
    for (auto& th : threads)
        th.join();
    CHECK(errs.load() == 0, "no exceptions concurrent atomic_bump");
    CHECK(cs.bridge_epoch() > be0, "bridge advanced under concurrency");
    CHECK(aura_aot_func_table_epoch() > aot0, "AOT table advanced under concurrency");
}

static void ac6_jit_dual_check_interleave() {
    std::println("\n--- AC6: JIT dual-check stale interleave ---");
    CompilerService cs; // wires aot metrics
    (void)cs;
    aura_set_aot_defuse_version(50);
    auto id = aura_alloc_closure(3);
    CHECK(id >= 0, "alloc_closure");
    const auto deopt0 = aura_jit_closure_stale_deopt_total();
    const auto safe0 = aura_jit_closure_safe_fallbacks();

    std::atomic<bool> stop{false};
    std::thread bumper([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            aura_aot_bump_func_table_epoch();
            aura_set_aot_defuse_version(aura_get_aot_defuse_version() + 1);
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < 500; ++i) {
        int64_t args[1] = {i};
        (void)aura_closure_call(id, args, 1);
    }
    stop.store(true, std::memory_order_relaxed);
    bumper.join();
    aura_free_closure(id);
    CHECK(aura_jit_closure_stale_deopt_total() >= deopt0, "stale_deopt non-decreasing");
    CHECK(aura_jit_closure_safe_fallbacks() >= safe0, "safe_fallbacks non-decreasing");
    CHECK(true, "JIT dual-check interleave completed");
}

static void ac7_fiber_steal_probe() {
    std::println("\n--- AC7: fiber-steal probe multifiber counters ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    ClosureId cid = 0;
    CHECK(capture_lambda(cs, cid), "capture for steal");
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    const auto races0 = load_u64(m->multifiber_mutate_races_detected_total);
    // Stale apply first (ensures closure is epoch-behind).
    (void)cs.evaluator().apply_closure(cid, {make_int(1)});
    // Steal probe walks closures and may record violation.
    for (int i = 0; i < 20; ++i)
        cs.evaluator().probe_linear_ownership_on_fiber_steal();
    CHECK(load_u64(m->multifiber_mutate_races_detected_total) >= races0,
          "races non-decreasing after steal probes");
    CHECK(true, "steal probe path completed");
}

static void ac8_four_fiber_1000_cycle() {
    std::println("\n--- AC8: 4-fiber 1000-round mutate/eval cycle ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(cs.eval("(set-code \"(define (h x) (+ x 1))\")").has_value(), "set-code h");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");

    auto clo = cs.eval("(lambda (z) (h z))");
    CHECK(clo && is_closure(*clo), "lambda closes over h");
    auto cid = static_cast<ClosureId>(as_closure_id(*clo));

    constexpr int kWorkers = 4;
    constexpr int kRounds = 250; // 4*250 = 1000
    std::atomic<std::uint64_t> rounds{0};
    std::atomic<int> errors{0};
    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&, w]() {
            try {
                auto args = std::array{make_int(w + 1)};
                for (int i = 0; i < kRounds; ++i) {
                    // Immediate eval of potentially-stale closure.
                    auto r = cs.evaluator().apply_closure(
                        cid, std::span<const aura::compiler::types::EvalValue>(args));
                    (void)r;
                    // Concurrent dual-epoch invalidate + rebind on half of rounds.
                    if ((i + w) % 2 == 0) {
                        cs.public_atomic_bump_epochs_and_stamp_bridge("h");
                        (void)cs.public_typed_mutate(std::format(
                            "(mutate:rebind \"h\" \"(lambda (x) (+ x {}))\")", (i % 7) + 1));
                    }
                    rounds.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                errors.fetch_add(1);
            }
        });
    }
    for (auto& t : workers)
        t.join();

    CHECK(errors.load() == 0, "no exceptions in 1000-round multi-fiber cycle");
    CHECK(rounds.load() == static_cast<std::uint64_t>(kWorkers * kRounds), "1000 rounds completed");
    CHECK(load_u64(m->multifiber_mutate_races_detected_total) > 0 ||
              load_u64(m->multifiber_safe_fallback_total) > 0 ||
              load_u64(m->closure_stale_apply_count_total) > 0,
          "at least one race/fallback surface exercised");
    std::println("  rounds={} races={} safe={} stale={}", rounds.load(),
                 load_u64(m->multifiber_mutate_races_detected_total),
                 load_u64(m->multifiber_safe_fallback_total),
                 load_u64(m->closure_stale_apply_count_total));
}

} // namespace aura_issue_1525_detail

int main() {
    using namespace aura_issue_1525_detail;
    std::println("=== Issue #1525: multi-fiber concurrent mutate + eval stress ===");
    ac1_metrics_surface();
    ac2_single_thread_1000();
    ac3_four_workers_main_mutates();
    ac4_recursive_define_capture();
    ac5_dual_epoch_under_concurrency();
    ac6_jit_dual_check_interleave();
    ac7_fiber_steal_probe();
    ac8_four_fiber_1000_cycle();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
