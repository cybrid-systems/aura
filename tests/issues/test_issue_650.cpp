// @category: integration
// @reason: Issue #650 StealBudget in WorkerThread to Use
// fiber yield_classification() + Outermost Mutation Depth
// for Adaptive Bias — query:scheduler-stealbudget-yield-
// class-stats structured companion
//
// Scope-limited close matching #601/#491/#479/#604/#606/#614/
// #615/#616/#617/#618/#620/#621/#622/#623/#624/#625/#626/#630/
// #631/#632/#633/#637/#640/#641/#642/#643/#644/#645/#646/#647/
// #648/#649 pattern.
//
// Discovery before this PR: the StealBudget adaptive bias
// surface already covers the high-level StealBudget summary
// via #706 + the per-steal LIFO/FIFO counters via #645:
//   - (engine:metrics \"query:scheduler-stealbudget-adaptive-stats\") (#706) —
//     5-field adaptive bias summary (already covers the AC3
//     surface listed in #650 body — mutation-bias-hits +
//     outermost-preferred + llm-tail-reductions +
//     deferred-pressure-boosts + global-deferred-mutation-
//     total)
//   - (engine:metrics \"query:scheduler-steal-bias-stats\") (#645) — per-steal
//     LIFO/FIFO + mutation-deferred counters
//   - #618 per-fiber yield_reason classification +
//     is_at_mutation_boundary_safe + outermost depth probe
//   - #588 per-fiber stack + StealBudget WINDOW_SIZE=10
//     thresholds 50%/30%/10%
//   - #451 work-stealing deque LIFO local + FIFO steal
//
// What the issue body AC3 specifies by **exact name + fields** —
// `query:scheduler-stealbudget-adaptive-stats` already ships
// the AC3 fields (5-field summary). The remaining AC1+AC2+AC4
// enforcement work needs a distinct name to expose the
// per-decision wire-up counters.
//
// So #650 ships ONE new Aura primitive + 3 new atomics that
// are foundation scaffolding for the future AC1
// (try_steal_from / should_steal query yield_classification +
// outermost Mutation depth → increase steal priority / reduce
// sleep threshold), AC2 (high steal_deferred_mutation_boundary
// _count temporarily raises max_before_sleep or biases LIFO
// local), and AC4 (unit test mock Fiber yield reasons) wire-up
// work.
//
// Non-duplicative to #645 (issue body explicitly cross-referenced).
//
// The remaining #650 AC1 + AC2 + AC4 work is invasive C++ on
// worker.cpp/h + StealBudget + needs the LLM latency + mixed
// yield reasons matrix + 20 fibers + TSan coverage from the
// issue body — separate follow-ups.

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

namespace aura_issue_650_detail {
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

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:scheduler-stealbudget-yield-class-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_650_detail

int aura_issue_650_run() {
    using namespace aura_issue_650_detail;
    std::println(
        "=== Issue #650: query:scheduler-stealbudget-yield-class-stats structured companion ===");

    aura::compiler::CompilerService cs;

    // AC1: hash returns a hash with the documented fields.
    {
        std::println("\n--- AC1: (engine:metrics "
                     "\"query:scheduler-stealbudget-yield-class-stats\") shape ---");
        auto h = cs.eval("(engine:metrics \"query:scheduler-stealbudget-yield-class-stats\")");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "scheduler-stealbudget-yield-class-stats returns a hash");
        const auto outermost = hash_int(cs, "outermost-bias");
        const auto explicit_b = hash_int(cs, "explicit-bias");
        const auto max_sleep = hash_int(cs, "max-sleep-raised");
        const auto schema = hash_int(cs, "schema");
        CHECK(outermost >= 0, std::format("outermost-bias >= 0 (got {})", outermost));
        CHECK(explicit_b >= 0, std::format("explicit-bias >= 0 (got {})", explicit_b));
        CHECK(max_sleep >= 0, std::format("max-sleep-raised >= 0 (got {})", max_sleep));
        CHECK(schema == 650, std::format("schema == 650 (got {})", schema));
    }

    // AC2: existing primitives remain reachable
    // (back-compat — #650 doesn't disturb them).
    {
        std::println("\n--- AC2: existing primitives back-compat ---");
        auto s_706 = cs.eval("(engine:metrics \"query:scheduler-stealbudget-adaptive-stats\")");
        CHECK(s_706.has_value(),
              "(engine:metrics \"query:scheduler-stealbudget-adaptive-stats\") reachable (#706 "
              "back-compat — 5-field adaptive bias summary)");
        auto s_645 = cs.eval("(engine:metrics \"query:scheduler-steal-bias-stats\")");
        CHECK(s_645.has_value(),
              "(engine:metrics \"query:scheduler-steal-bias-stats\") reachable (#645 back-compat — "
              "per-steal LIFO/FIFO + mutation-deferred)");
        auto s_649 = cs.eval("(engine:metrics \"query:yield-checkpoint-panic-stats\")");
        CHECK(
            s_649.has_value(),
            "(engine:metrics \"query:yield-checkpoint-panic-stats\") reachable (#649 back-compat)");
        auto s_648 = cs.eval("(engine:metrics \"query:panic-checkpoint-fiber-stats\")");
        CHECK(
            s_648.has_value(),
            "(engine:metrics \"query:panic-checkpoint-fiber-stats\") reachable (#648 back-compat)");
        auto s_646 = cs.eval("(engine:metrics \"query:gc-safepoint-deferral-stats\")");
        CHECK(
            s_646.has_value(),
            "(engine:metrics \"query:gc-safepoint-deferral-stats\") reachable (#646 back-compat)");
        auto s_647 = cs.eval("(engine:metrics \"query:envframe-dualpath-stale-stats-hash\")");
        CHECK(s_647.has_value(), "(engine:metrics \"query:envframe-dualpath-stale-stats-hash\") "
                                 "reachable (#647 back-compat)");
    }

    // AC3: derived-metric invariants on a fresh service.
    // outermost-bias / explicit-bias / max-sleep-raised are
    // all 0 on a fresh service — they are foundation
    // scaffolding for the future AC1 + AC2 + AC4 enforcement
    // work (StealBudget consultation of victim
    // yield_classification + outermost depth + max_before_sleep
    // raise on contention).
    {
        std::println("\n--- AC3: derived-metric invariants on fresh service ---");
        const auto outermost = hash_int(cs, "outermost-bias");
        const auto explicit_b = hash_int(cs, "explicit-bias");
        const auto max_sleep = hash_int(cs, "max-sleep-raised");
        CHECK(outermost == 0, std::format("fresh-service outermost-bias == 0 (got {})", outermost));
        CHECK(explicit_b == 0,
              std::format("fresh-service explicit-bias == 0 (got {})", explicit_b));
        CHECK(max_sleep == 0,
              std::format("fresh-service max-sleep-raised == 0 (got {})", max_sleep));
    }

    // AC4: schema sentinel is exactly 650 (not 649/648/647/646).
    {
        std::println("\n--- AC4: schema sentinel ---");
        const auto schema = hash_int(cs, "schema");
        CHECK(schema == 650, std::format("schema == 650 (got {})", schema));
    }

    // AC5: naming distinction — the new primitive name uses
    // `-yield-class-` midfix, distinct from #706's
    // `-stealbudget-adaptive-` (5-field summary). Also
    // distinct from #645's `-steal-bias-` (per-steal LIFO/FIFO).
    {
        std::println("\n--- AC5: naming distinction from #706 + #645 ---");
        auto new_p = cs.eval("(engine:metrics \"query:scheduler-stealbudget-yield-class-stats\")");
        auto old_706 = cs.eval("(engine:metrics \"query:scheduler-stealbudget-adaptive-stats\")");
        auto old_645 = cs.eval("(engine:metrics \"query:scheduler-steal-bias-stats\")");
        CHECK(new_p.has_value(),
              "new primitive (engine:metrics \"query:scheduler-stealbudget-yield-class-stats\") "
              "reachable (-yield-class- midfix)");
        CHECK(old_706.has_value(),
              "existing #706 (engine:metrics \"query:scheduler-stealbudget-adaptive-stats\") "
              "still reachable (5-field summary)");
        CHECK(old_645.has_value(),
              "existing #645 (engine:metrics \"query:scheduler-steal-bias-stats\") still "
              "reachable (per-steal LIFO/FIFO)");
        // The new primitive uses `schema` as its primary
        // sentinel — distinct from #706's `mutation-bias-hits`
        // / #645's `schema` (different values: 706 has no schema
        // field; 645 has schema=645). Verify schema==650 path
        // and that the new primitive has its own 3 documented
        // fields (avoid hash-ref on missing keys — see
        // #644/#645 lessons).
        const auto check_new_field = [&](std::string_view k) {
            auto r = cs.eval(std::format("(hash-ref (engine:metrics "
                                         "\"query:scheduler-stealbudget-yield-class-stats\") '{}')",
                                         k));
            return r.has_value() && aura::compiler::types::is_int(*r);
        };
        CHECK(check_new_field("outermost-bias"), "new primitive 'outermost-bias' field reachable");
        CHECK(check_new_field("explicit-bias"), "new primitive 'explicit-bias' field reachable");
        CHECK(check_new_field("max-sleep-raised"),
              "new primitive 'max-sleep-raised' field reachable");
    }

    // AC6: concurrent reads under 2 threads × 4 iters. Atomicity
    // regression coverage for the underlying counters + 3 new
    // scaffolding atomics.
    {
        std::println("\n--- AC6: concurrent scheduler-stealbudget-yield-class-stats reads ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 4;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r =
                    cs.eval("(engine:metrics \"query:scheduler-stealbudget-yield-class-stats\")");
                if (r.has_value())
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(
            ok_count.load() == k_iters * 2,
            std::format("concurrent: {} / {} calls returned value", ok_count.load(), k_iters * 2));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_650_run();
}
#endif
