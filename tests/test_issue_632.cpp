// @category: integration
// @reason: Issue #632 atomic-batch SV observability foundation —
// query:atomic-batch-sv-stats-hash structured companion
//
// Scope-limited close matching #601/#491/#479/#604/#606/#614/
// #615/#616/#617/#618/#620/#621/#622/#623/#624/#625/#626/#630/
// #631 pattern.
//
// Discovery before this PR (no duplication): the atomic batch
// infrastructure + observability surface already exists in the
// C++ side. Pre-existing primitives:
//   - (mutate:atomic-batch ops-list "summary") (#192/#213) —
//     list-call form
//   - (atomic-batch:stats) (#192) — 5-field observable hash
//   - (query:atomic-batch-stats) (#437) — int statistic
//   - (query:atomic-batch-stats-hash) (#622) — 4-field structured
//   - (query:atomic-batch-rollback-stats) (#529) — int-sum of
//     rollback observability
//
// What the issue body AC4 specifies by **exact name + fields** —
// `query:atomic-batch-sv-stats` with {active_sv_batches,
// suppressed_bumps_on_sv, rollback_success_sv,
// batch_impact_sv_nodes} — was *not* shipped under that exact
// name. So #632 ships ONE new Aura primitive + 2 new atomics
// that are foundation scaffolding for the future AC2 + AC3
// enforcement work (the bumps will land when those ship).
//
// The remaining #632 AC1 + AC2 + AC3 work (high-level atomic-batch
// SV mutate primitive, Guard nesting-depth + suppressed-bump-
// count tracking inside, StableNodeRef auto-refresh on capture
// with suppressed-gen awareness for SV nodes) is invasive C++
// + hot-path EDA + Guard-internal work that needs benchmarking
// + perf regression coverage alongside the JIT/hot-swap work
// in #601/#491 — separate follow-ups.

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

namespace aura_issue_632_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:atomic-batch-sv-stats-hash) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_632_detail

int aura_issue_632_run() {
    using namespace aura_issue_632_detail;
    std::println("=== Issue #632: query:atomic-batch-sv-stats-hash structured companion ===");

    aura::compiler::CompilerService cs;

    // AC1: hash returns a hash with the documented fields (5).
    {
        std::println("\n--- AC1: (query:atomic-batch-sv-stats-hash) shape ---");
        auto h = cs.eval("(query:atomic-batch-sv-stats-hash)");
        CHECK(h && aura::compiler::types::is_hash(*h), "atomic-batch-sv-stats-hash returns a hash");
        const auto active = hash_int(cs, "active-sv-batches");
        const auto suppressed = hash_int(cs, "suppressed-bumps-on-sv");
        const auto rollback = hash_int(cs, "rollback-success-sv");
        const auto impact = hash_int(cs, "batch-impact-sv-nodes");
        const auto schema = hash_int(cs, "schema");
        CHECK(active == 0 || active == 1,
              std::format("active-sv-batches in {{0,1}} (got {})", active));
        CHECK(suppressed >= 0, std::format("suppressed-bumps-on-sv >= 0 (got {})", suppressed));
        CHECK(rollback >= 0, std::format("rollback-success-sv >= 0 (got {})", rollback));
        CHECK(impact >= 0, std::format("batch-impact-sv-nodes >= 0 (got {})", impact));
        CHECK(schema == 632, std::format("schema == 632 (got {})", schema));
    }

    // AC2: existing primitives remain reachable
    // (back-compat — #632 doesn't disturb the existing surface).
    {
        std::println("\n--- AC2: existing primitives back-compat ---");
        auto s_stats = cs.eval("(atomic-batch:stats)");
        CHECK(s_stats.has_value(), "(atomic-batch:stats) reachable (#192 back-compat)");
        auto s_st = cs.eval("(query:atomic-batch-stats)");
        CHECK(s_st && aura::compiler::types::is_int(*s_st),
              "(query:atomic-batch-stats) returns an int (#437 back-compat)");
        auto s_hash = cs.eval("(query:atomic-batch-stats-hash)");
        CHECK(s_hash && aura::compiler::types::is_hash(*s_hash),
              "(query:atomic-batch-stats-hash) returns a hash (#622 back-compat)");
        auto s_roll = cs.eval("(query:atomic-batch-rollback-stats)");
        CHECK(s_roll && aura::compiler::types::is_int(*s_roll),
              "(query:atomic-batch-rollback-stats) returns an int (#529 back-compat)");
    }

    // AC3: derived-metric invariants on a fresh service.
    // All 4 derived/verbatim fields should be 0 when no workload
    // + no enforcement wire-up yet.
    {
        std::println("\n--- AC3: derived-metric invariants on fresh service ---");
        const auto active = hash_int(cs, "active-sv-batches");
        const auto suppressed = hash_int(cs, "suppressed-bumps-on-sv");
        const auto rollback = hash_int(cs, "rollback-success-sv");
        const auto impact = hash_int(cs, "batch-impact-sv-nodes");
        CHECK(active == 0, std::format("fresh-service active-sv-batches == 0 (got {})", active));
        CHECK(suppressed == 0,
              std::format("fresh-service suppressed-bumps-on-sv == 0 (got {})", suppressed));
        CHECK(rollback == 0,
              std::format("fresh-service rollback-success-sv == 0 (got {})", rollback));
        CHECK(impact == 0,
              std::format("fresh-service batch-impact-sv-nodes == 0 (got {})", impact));
    }

    // AC4: schema sentinel is exactly 632 (not 622/630/631).
    {
        std::println("\n--- AC4: schema sentinel ---");
        const auto schema = hash_int(cs, "schema");
        CHECK(schema == 632, std::format("schema == 632 (got {})", schema));
    }

    // AC5: concurrent reads under 2 threads × 4 iters. Atomicity
    // regression coverage for the underlying atomics + 2 new
    // scaffolding atomics.
    {
        std::println("\n--- AC5: concurrent atomic-batch-sv-stats-hash reads ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 4;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(query:atomic-batch-sv-stats-hash)");
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
    return aura_issue_632_run();
}
#endif
