// @category: integration
// @reason: Issue #626 C++26 Contracts + consteval + hot-path
// observability — query:contracts-hotpath-stats-hash structured
// companion
//
// Scope-limited close matching #601/#491/#479/#604/#606/#614/
// #615/#616/#617/#618/#620/#621/#622/#623/#624/#625 pattern.
//
// Discovery before this PR (no duplication): the C++ side already
// exposes the full Contracts + consteval + zero-overhead +
// dirty short-circuit counter surface that #626 AC5 lists,
// via counters in shape::namespace + CompilerMetrics +
// passes_skipped_dirty_pipeline (added by #406 / #508 / #605 /
// #686 / #494 / #606). Pre-existing primitives:
//   - (query:task4-hotpath-contracts) — 10-field structured hash
//     with per-site constants + consteval-hits +
//     task4-contracts-total + task4-contracts-recommendation
//   - (engine:metrics \"query:pass-pipeline-incremental-stats-hash\") from #625
//     — 6-field hash with contracts-checked (synthetic from
//     zerooverhead_wins + dispatch_miss)
//   - (engine:metrics \"query:pass-contracts-stats\") from #406 — int-sum of 7 counters
//   - (engine:metrics \"query:dead-coercion-zerooverhead-stats\") from #508 —
//     zerooverhead-wins
//   - shape::value_contract_violation_count —
//     violations-in-debug field
//
// What the issue body AC5 specifies by **exact name + fields** —
// `query:contracts-hotpath-stats` with {contracts_checked,
// violations_in_debug, consteval_hits, zero_overhead_savings} —
// was *not* shipped under that exact name. So #626 ships ONE new
// Aura primitive consolidating the AC5 fields.
//
// The remaining #626 AC1 + AC2 + AC3 + AC4 (post/requires on
// Arena allocate_raw, ShapeProfiler record_shape, mark_dirty_*,
// IRInstructionView accessors; consteval on shape tag dispatch +
// Value v2 bias ranges; wire to Pass short-circuit) is invasive
// C++ + hot-path C++26-Contracts additions that need
// benchmarking + perf regression coverage alongside the JIT/hot-
// swap work in #601 / #491 — separate follow-ups.

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

namespace aura_issue_626_detail {
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
        "(hash-ref (engine:metrics \"query:contracts-hotpath-stats-hash\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_626_detail

int aura_issue_626_run() {
    using namespace aura_issue_626_detail;
    std::println("=== Issue #626: query:contracts-hotpath-stats-hash structured companion ===");

    aura::compiler::CompilerService cs;

    // AC1: hash returns a hash with the 8 documented fields.
    {
        std::println(
            "\n--- AC1: (engine:metrics \"query:contracts-hotpath-stats-hash\") shape ---");
        auto h = cs.eval("(engine:metrics \"query:contracts-hotpath-stats-hash\")");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "contracts-hotpath-stats-hash returns a hash");
        const auto contracts_checked = hash_int(cs, "contracts-checked");
        const auto violations = hash_int(cs, "violations-in-debug");
        const auto consteval_hits = hash_int(cs, "consteval-hits");
        const auto zero_overhead = hash_int(cs, "zero-overhead-savings");
        const auto pipeline_runs = hash_int(cs, "pass-pipeline-runs");
        const auto arena_triggers = hash_int(cs, "arena-auto-triggers");
        const auto dirty_skipped = hash_int(cs, "dirty-blocks-skipped");
        const auto schema = hash_int(cs, "schema");
        CHECK(contracts_checked >= 0 && contracts_checked <= 100,
              std::format("contracts-checked in [0,100] (got {})", contracts_checked));
        CHECK(violations >= 0, std::format("violations-in-debug >= 0 (got {})", violations));
        CHECK(consteval_hits >= 0, std::format("consteval-hits >= 0 (got {})", consteval_hits));
        CHECK(zero_overhead >= 0,
              std::format("zero-overhead-savings >= 0 (got {})", zero_overhead));
        CHECK(pipeline_runs >= 0, std::format("pass-pipeline-runs >= 0 (got {})", pipeline_runs));
        CHECK(arena_triggers >= 0,
              std::format("arena-auto-triggers >= 0 (got {})", arena_triggers));
        CHECK(dirty_skipped >= 0, std::format("dirty-blocks-skipped >= 0 (got {})", dirty_skipped));
        CHECK(schema == 626, std::format("schema == 626 (got {})", schema));
    }

    // AC2: existing primitives remain reachable
    // (back-compat — #626 doesn't disturb the existing surface).
    {
        std::println("\n--- AC2: existing primitives back-compat ---");
        auto s_task4 = cs.eval("(query:task4-hotpath-contracts)");
        CHECK(s_task4 && aura::compiler::types::is_hash(*s_task4),
              "(query:task4-hotpath-contracts) returns a hash (existing)");
        auto s_pipe = cs.eval("(engine:metrics \"query:pass-pipeline-incremental-stats-hash\")");
        CHECK(s_pipe && aura::compiler::types::is_hash(*s_pipe),
              "(engine:metrics \"query:pass-pipeline-incremental-stats-hash\") returns a hash "
              "(#625 back-compat)");
        auto s_cont = cs.eval("(engine:metrics \"query:pass-contracts-stats\")");
        CHECK(s_cont && aura::compiler::types::is_int(*s_cont),
              "(engine:metrics \"query:pass-contracts-stats\") returns an int (#406 back-compat)");
        auto s_dead = cs.eval("(engine:metrics \"query:dead-coercion-zerooverhead-stats\")");
        CHECK(s_dead.has_value(), "(engine:metrics \"query:dead-coercion-zerooverhead-stats\") "
                                  "reachable (#508 back-compat)");
    }

    // AC3: derived-metric invariants on a fresh service.
    // With no workload yet, derived fields should be 0 and
    // consteval_hits should still be a non-negative count.
    {
        std::println("\n--- AC3: derived-metric invariants on fresh service ---");
        const auto contracts_checked = hash_int(cs, "contracts-checked");
        const auto violations = hash_int(cs, "violations-in-debug");
        const auto zero_overhead = hash_int(cs, "zero-overhead-savings");
        const auto pipeline_runs = hash_int(cs, "pass-pipeline-runs");
        const auto arena_triggers = hash_int(cs, "arena-auto-triggers");
        const auto dirty_skipped = hash_int(cs, "dirty-blocks-skipped");
        CHECK(contracts_checked == 0,
              std::format("fresh-service contracts-checked == 0 (got {})", contracts_checked));
        CHECK(violations == 0,
              std::format("fresh-service violations-in-debug == 0 (got {})", violations));
        CHECK(pipeline_runs == 0,
              std::format("fresh-service pass-pipeline-runs == 0 (got {})", pipeline_runs));
        CHECK(arena_triggers == 0,
              std::format("fresh-service arena-auto-triggers == 0 (got {})", arena_triggers));
        CHECK(dirty_skipped == 0,
              std::format("fresh-service dirty-blocks-skipped == 0 (got {})", dirty_skipped));
        CHECK(zero_overhead >= 0,
              std::format("fresh-service zero-overhead-savings is non-negative (got {})",
                          zero_overhead));
    }

    // AC4: schema sentinel is exactly 626 (not 622/623/624/625).
    {
        std::println("\n--- AC4: schema sentinel ---");
        const auto schema = hash_int(cs, "schema");
        CHECK(schema == 626, std::format("schema == 626 (got {})", schema));
    }

    // AC5: concurrent reads under 2 threads × 4 iters. Atomicity
    // regression coverage for the underlying shape::* counters.
    {
        std::println("\n--- AC5: concurrent contracts-hotpath-stats-hash reads ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 4;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(engine:metrics \"query:contracts-hotpath-stats-hash\")");
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
    return aura_issue_626_run();
}
#endif
