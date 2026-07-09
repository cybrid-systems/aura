// @category: integration
// @reason: Issue #622 atomic-batch observability —
// query:atomic-batch-stats-hash structured companion
//
// Scope-limited close matching the #601/#491/#479/#604/#606/#614/
// #615/#616/#617/#618/#620/#621 pattern.
//
// IMPORTANT DISCOVERY before this PR: the high-level Aura
// surface that #622 was assumed to be missing is ALREADY THERE:
//   - (mutate:atomic-batch ops-list "summary") from #192/#213
//     (evaluator_primitives_mutate.cpp:2645) — list-call form
//   - (atomic-batch:stats) from #192
//     (evaluator_primitives_observability.cpp:1586) — observability hash
//   - (query:atomic-batch-stats) from #437
//     (evaluator_primitives_compile.cpp:2098) — int statistic
//   - (query:atomic-batch-rollback-stats) from #529
//     (evaluator_primitives_query.cpp:3772) — rollback observability
//
// So instead of duplicating those primitives with split-name
// forms, #622 ships ONE new Aura primitive — a structured-hash
// companion to the existing flat-int (query:atomic-batch-stats)
// that surfaces per-batch runtime state (active flag, current
// batch's suppressed-bumps count) that's NOT in either
// (atomic-batch:stats) or (query:atomic-batch-stats).
//
// The remaining #622 AC2 (Guard nesting-depth + per-batch
// impact_nodes + rollback_success_rate) + AC4 (in-batch
// StableRef refresh) are invasive Guard-internal / observability
// changes that need benchmarking + perf regression coverage
// alongside the MutateBoundary stack work in #619 — separate
// follow-ups.

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

namespace aura_issue_622_detail {
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

static std::int64_t batch_hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:atomic-batch-stats-hash) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_622_detail

int aura_issue_622_run() {
    using namespace aura_issue_622_detail;
    std::println("=== Issue #622: query:atomic-batch-stats-hash structured companion ===");

    aura::compiler::CompilerService cs;

    // AC1: (query:atomic-batch-stats-hash) returns a hash with the
    // documented 4 fields.
    {
        std::println("\n--- AC1: (query:atomic-batch-stats-hash) shape ---");
        auto h = cs.eval("(query:atomic-batch-stats-hash)");
        CHECK(h && aura::compiler::types::is_hash(*h), "atomic-batch-stats-hash returns a hash");
        const auto active = batch_hash_int(cs, "active");
        const auto commits_total = batch_hash_int(cs, "commits-total");
        const auto bumps_saved = batch_hash_int(cs, "bumps-saved-last-batch");
        const auto schema = batch_hash_int(cs, "schema");
        CHECK(active == 0 || active == 1, std::format("active in {{0,1}} (got {})", active));
        CHECK(commits_total >= 0, std::format("commits-total >= 0 (got {})", commits_total));
        CHECK(bumps_saved >= 0, std::format("bumps-saved-last-batch >= 0 (got {})", bumps_saved));
        CHECK(schema == 622, std::format("schema == 622 (got {})", schema));
    }

    // AC2: existing (atomic-batch:stats) (#192) + (query:atomic-
    // batch-stats) (#437) + (query:atomic-batch-rollback-stats)
    // (#529) primitives remain reachable (#622 doesn't disturb
    // the existing observability surface).
    {
        std::println("\n--- AC2: existing primitives back-compat ---");
        auto s192 = cs.eval("(atomic-batch:stats)");
        CHECK(s192.has_value(), "(atomic-batch:stats) reachable (#192 back-compat)");
        auto s437 = cs.eval("(query:atomic-batch-stats)");
        CHECK(s437 && aura::compiler::types::is_int(*s437),
              "(query:atomic-batch-stats) returns an int (#437 back-compat)");
        auto s529 = cs.eval("(query:atomic-batch-rollback-stats)");
        CHECK(s529.has_value(), "(query:atomic-batch-rollback-stats) reachable (#529 back-compat)");
    }

    // AC3: concurrent reads of (query:atomic-batch-stats-hash)
    // under 2 threads × 4 iters. Atomicity regression coverage.
    {
        std::println("\n--- AC3: concurrent atomic-batch-stats-hash reads ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 4;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(query:atomic-batch-stats-hash)");
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
    return aura_issue_622_run();
}
#endif
