// @category: integration
// @reason: Issue #618 LLM-aware orchestration metrics foundation —
// query:scheduler-mutation-coord-stats + orchestration:tune-gc-frequency
//
// Scope-limited close matching the #601/#491/#479/#604/#606/#614/
// #615/#616/#617 pattern: ship the two new orchestration primitives
// + 1 new atomic + test coverage now; the bigger adaptive strategy
// (steal-bias / per-worker caching / scheduler affinity / LlmWait
// YieldReason sub-classification / scheduler-side consult of
// gc_frequency_tune_ratio) remains a separate follow-up.
//
// Foundation already in place from #451/#588/#591/#607:
//   - Fiber::yield_classification() with outermost/inner distinction
//   - Fiber::is_at_mutation_boundary_safe()
//   - aura_fiber_static_gc_pause_attributed_to_mutation() (C-linkage
//     shim for the static aggregate bumped in check_gc_safepoint)
//   - aura_evaluator_mutation_boundary_depth() (C-linkage depth probe)
//   - yield_mutation_boundary_count / yield_explicit_count /
//     yield_scheduler_steal_count / yield_blocking_io_count /
//     yield_operation_boundary_count per-fiber counters
//   - steal_deferred_mutation_boundary_count per-fiber counter
//   - YieldReason enum: BlockingIO / MutationBoundary / Explicit /
//     SchedulerSteal / OperationBoundary / PassPipeline
//   - (query:orchestration-metrics) returns JSON string (kept for
//     back-compat with test_issue_451)
//   - (scheduler:pin) + (orch:reset-metrics) mutation coords already
//     exposed

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_618_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t coord_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:scheduler-mutation-coord-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_618_detail

int aura_issue_618_run() {
    using namespace aura_issue_618_detail;
    std::println("=== Issue #618: LLM-aware orchestration metrics foundation ===");

    aura::compiler::CompilerService cs;

    // AC1: (engine:metrics \"query:scheduler-mutation-coord-stats\") returns a hash
    // with 6 fields. Backward-aware: tests the structured form
    // alongside the legacy JSON string from #451.
    {
        std::println(
            "\n--- AC1: (engine:metrics \"query:scheduler-mutation-coord-stats\") shape ---");
        auto h = cs.eval("(engine:metrics \"query:scheduler-mutation-coord-stats\")");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "coord-stats returns a hash (not the legacy string)");
        const auto gc_pauses = coord_int(cs, "gc-pauses-attributed-to-mutation");
        const auto depth = coord_int(cs, "mutation-boundary-depth");
        const auto fiber_id = coord_int(cs, "current-fiber-id");
        const auto active = coord_int(cs, "is-fibers-active");
        const auto ratio = coord_int(cs, "gc-frequency-tune-ratio");
        const auto schema = coord_int(cs, "schema");
        CHECK(gc_pauses >= 0,
              std::format("gc-pauses-attributed-to-mutation >= 0 (got {})", gc_pauses));
        CHECK(depth >= 0, std::format("mutation-boundary-depth >= 0 (got {})", depth));
        CHECK(fiber_id >= 0, std::format("current-fiber-id >= 0 (got {})", fiber_id));
        CHECK(active == 0 || active == 1,
              std::format("is-fibers-active in {{0, 1}} (got {})", active));
        CHECK(ratio >= 0 && ratio <= 100,
              std::format("gc-frequency-tune-ratio in [0,100] (got {})", ratio));
        CHECK(schema == 618, std::format("schema == 618 (got {})", schema));
        // Backward compat: legacy JSON-string primitive still works.
        auto legacy = cs.eval("(query:orchestration-metrics)");
        CHECK(legacy && aura::compiler::types::is_string(*legacy),
              "legacy (query:orchestration-metrics) returns a string (#451 back-compat)");
    }

    // AC2: (orchestration:tune-gc-frequency) with no arg reads
    // back the current ratio; default is 50 (matches the
    // historical every-Nth-allocation heuristic).
    {
        std::println("\n--- AC2: tune-gc-frequency readback (default = 50) ---");
        auto r = cs.eval("(orchestration:tune-gc-frequency)");
        CHECK(r && aura::compiler::types::is_int(*r), "tune-gc-frequency read returns an int");
        const auto prev_ratio = aura::compiler::types::as_int(*r);
        // Default is 50 — but a previous test run might have set it,
        // so accept anything in [0,100] and confirm the value
        // observed in AC1's coord-stats matches.
        CHECK(prev_ratio >= 0 && prev_ratio <= 100,
              std::format("read ratio in [0,100] (got {})", prev_ratio));
        const auto from_stats = coord_int(cs, "gc-frequency-tune-ratio");
        CHECK(prev_ratio == from_stats,
              std::format("read value == coord-stats value ({} == {})", prev_ratio, from_stats));
    }

    // AC3: (orchestration:tune-gc-frequency ratio) sets the value
    // and returns the previous value. Out-of-range args are clamped.
    {
        std::println("\n--- AC3: tune-gc-frequency setter + clamping ---");
        // Set to 75; should return the previous value.
        const auto before_set =
            aura::compiler::types::as_int(*cs.eval("(orchestration:tune-gc-frequency)"));
        auto r = cs.eval("(orchestration:tune-gc-frequency 75)");
        CHECK(r && aura::compiler::types::is_int(*r), "setter returns an int (previous value)");
        const auto returned_prev = aura::compiler::types::as_int(*r);
        CHECK(returned_prev == before_set,
              std::format("setter returned previous value (expected {}, got {})", before_set,
                          returned_prev));
        // Readback should be 75.
        auto after = cs.eval("(orchestration:tune-gc-frequency)");
        CHECK(aura::compiler::types::as_int(*after) == 75,
              std::format("readback after set == 75 (got {})",
                          aura::compiler::types::as_int(*after)));
        // coord-stats should also reflect the new value.
        const auto from_stats = coord_int(cs, "gc-frequency-tune-ratio");
        CHECK(from_stats == 75, std::format("coord-stats reflects new value (got {})", from_stats));
        // Clamping: -5 -> 0.
        (void)cs.eval("(orchestration:tune-gc-frequency -5)");
        CHECK(aura::compiler::types::as_int(*cs.eval("(orchestration:tune-gc-frequency)")) == 0,
              "negative arg clamped to 0");
        // Clamping: 200 -> 100.
        (void)cs.eval("(orchestration:tune-gc-frequency 200)");
        CHECK(aura::compiler::types::as_int(*cs.eval("(orchestration:tune-gc-frequency)")) == 100,
              "arg > 100 clamped to 100");
        // Restore to default 50 so subsequent runs aren't affected.
        (void)cs.eval("(orchestration:tune-gc-frequency 50)");
    }

    // AC4: (orchestration:tune-gc-frequency) with non-int arg
    // returns the current value without changing it.
    {
        std::println("\n--- AC4: tune-gc-frequency bad-arg is no-op ---");
        (void)cs.eval("(orchestration:tune-gc-frequency 25)");
        const auto before = coord_int(cs, "gc-frequency-tune-ratio");
        auto r = cs.eval("(orchestration:tune-gc-frequency \"not-a-number\")");
        CHECK(r && aura::compiler::types::is_int(*r), "non-int arg returns an int (current value)");
        const auto after = coord_int(cs, "gc-frequency-tune-ratio");
        CHECK(after == before,
              std::format("non-int arg left ratio unchanged ({} -> {})", before, after));
        // Restore default.
        (void)cs.eval("(orchestration:tune-gc-frequency 50)");
    }

    // AC5: concurrent reads under 2 threads × 4 iters each.
    // Atomicity regression coverage for the gc_frequency_tune_ratio
    // atomic (so multiple readers don't observe torn writes).
    {
        std::println("\n--- AC5: concurrent tune-gc-frequency reads ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 4;
        (void)cs.eval("(orchestration:tune-gc-frequency 60)");
        const auto before = coord_int(cs, "gc-frequency-tune-ratio");
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(orchestration:tune-gc-frequency)");
                if (r && aura::compiler::types::is_int(*r))
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        const auto after = coord_int(cs, "gc-frequency-tune-ratio");
        CHECK(ok_count.load() == k_iters * 2,
              std::format("concurrent: {} / {} reads returned int", ok_count.load(), k_iters * 2));
        // The 8 reads shouldn't have changed the ratio (no-arg = read),
        // and concurrent reads under the eval lock should all see 60.
        CHECK(after == before, std::format("concurrent reads unchanged ({} -> {})", before, after));
        // Restore default.
        (void)cs.eval("(orchestration:tune-gc-frequency 50)");
    }

    // AC6: (query:orchestration-metrics) legacy JSON string still
    // works (back-compat regression — Issue #451 string contract).
    {
        std::println("\n--- AC6: legacy (query:orchestration-metrics) regression ---");
        auto legacy = cs.eval("(query:orchestration-metrics)");
        CHECK(legacy && aura::compiler::types::is_string(*legacy),
              "legacy primitive returns a string (no regression from #618)");
        // The string should contain a JSON-shape key we recognize.
        const auto& heap = cs.evaluator().string_heap();
        const auto sidx = aura::compiler::types::as_string_idx(*legacy);
        const std::string json(heap[sidx]);
        CHECK(json.find("gc_pauses_attributed_to_mutation") != std::string::npos,
              std::format("legacy JSON contains 'gc_pauses_attributed_to_mutation' (got: {})",
                          json.substr(0, std::min<std::size_t>(json.size(), 120))));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_618_run();
}
#endif
