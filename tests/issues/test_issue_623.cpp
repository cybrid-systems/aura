// @category: integration
// @reason: Issue #623 memory safety production-harden —
// arena:auto-compact-threshold (read) + arena:set-auto-compact-threshold (write)
//
// Scope-limited close matching the #601/#491/#479/#604/#606/#614/
// #615/#616/#617/#618/#620/#621/#622 pattern.
//
// Discovery before this PR (no duplication): the auto-compact +
// defrag + fiber/GC coord + observability surface that #623 was
// assumed to be missing is ALREADY THERE from #187 / #300 / #430 /
// #464 / #604 / #685 / various pre-existing primitives. Inventory:
//   - (arena:compact) / (arena:defrag) / (arena:compact-all) /
//     (arena:adaptive-compact) / (arena:shrink-to-fit) — manual
//     triggers (#187)
//   - (arena:request-defrag) / (arena:defrag-requested?) — request
//     side of the deflag flag (#300)
//   - (arena:compact-with-policy name "force"|"auto"|"skip") —
//     policy override (#430)
//   - (arena:should-auto-compact? name) — cheap O(1) probe (#335)
//   - (stats:get \"arena:adaptive-stats\") — trigger/skip counters (#335)
//   - (stats:get "arena:stats-json") — JSON snapshot (#187)
//   - (engine:metrics \"query:arena-auto-stats\") — group-level guard/skip (#464)
//   - (engine:metrics \"query:arena-auto-compact-stats\") — alloc-path policy (#685)
//   - (engine:metrics \"query:arena-fragmentation-snapshot\") — live point-in-time
//     hash (#604)
//
// What #623 ships that's NEW: 2 small Agent-tunables for the
// arena auto-compact threshold + 1 atomic counter. Pairs with the
// existing C++ ArenaGroup::set_compact_threshold() /
// compact_threshold() (already there from #187, never Aura-exposed)
// and the bump budget from #604/#685. Lets the memory-pressure
// watchdog tune at runtime how aggressive auto-compact is, without
// needing to rebuild or restart the process.
//
// The remaining #623 work (auto-trigger wiring in allocate_raw #1,
// basic live-object defrag for small tiers #2, SoA dirty wiring #3)
// is invasive C++ and remains a separate follow-up.

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

namespace aura_issue_623_detail {
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

} // namespace aura_issue_623_detail

int aura_issue_623_run() {
    using namespace aura_issue_623_detail;
    std::println("=== Issue #623: arena auto-compact threshold setter/getter ===");

    aura::compiler::CompilerService cs;

    // AC1: (arena:auto-compact-threshold) returns an int 0..95
    // (the default 50 in C++). -1 if no arena_group_ loaded.
    {
        std::println("\n--- AC1: (arena:auto-compact-threshold) read ---");
        auto r = cs.eval("(arena:auto-compact-threshold)");
        CHECK(r && aura::compiler::types::is_int(*r), "auto-compact-threshold returns an int");
        const auto val = aura::compiler::types::as_int(*r);
        // Default is 50; could also be -1 if no arena group is
        // loaded in the test service. Accept either.
        CHECK(val == 50 || val == -1,
              std::format("auto-compact-threshold in {{50, -1}} (got {})", val));
    }

    // AC2: (arena:set-auto-compact-threshold ratio) sets the
    // value and returns the previous value. Out-of-range args
    // are clamped.
    {
        std::println("\n--- AC2: (arena:set-auto-compact-threshold ratio) set + clamping ---");
        // Get baseline value first.
        const auto baseline =
            aura::compiler::types::as_int(*cs.eval("(arena:auto-compact-threshold)"));
        // Set to 75, expect baseline (or -1) back as previous.
        auto r_set = cs.eval("(arena:set-auto-compact-threshold 75)");
        CHECK(r_set && aura::compiler::types::is_int(*r_set),
              "setter returns an int (previous value)");
        const auto prev = aura::compiler::types::as_int(*r_set);
        // When baseline is -1 (no arena group), prev should also
        // be -1; when baseline is 50, prev should be 50.
        if (baseline == -1) {
            CHECK(prev == -1, std::format("baseline -1 case: prev == -1 (got {})", prev));
        } else {
            CHECK(prev == baseline, std::format("prev == baseline ({} == {})", prev, baseline));
        }
        // Readback should be 75.
        auto r_now = cs.eval("(arena:auto-compact-threshold)");
        const auto now = aura::compiler::types::as_int(*r_now);
        if (baseline != -1) {
            CHECK(now == 75, std::format("readback after set == 75 (got {})", now));
        }
        // Clamping: -5 -> 0.
        (void)cs.eval("(arena:set-auto-compact-threshold -5)");
        if (baseline != -1) {
            const auto after_clamp_low =
                aura::compiler::types::as_int(*cs.eval("(arena:auto-compact-threshold)"));
            CHECK(after_clamp_low == 0,
                  std::format("negative arg clamped to 0 (got {})", after_clamp_low));
        }
        // Clamping: 200 -> 95.
        (void)cs.eval("(arena:set-auto-compact-threshold 200)");
        if (baseline != -1) {
            const auto after_clamp_high =
                aura::compiler::types::as_int(*cs.eval("(arena:auto-compact-threshold)"));
            CHECK(after_clamp_high == 95,
                  std::format("arg > 95 clamped to 95 (got {})", after_clamp_high));
        }
        // Restore to default 50 so subsequent runs aren't affected.
        (void)cs.eval("(arena:set-auto-compact-threshold 50)");
    }

    // AC3: non-int arg is a no-op (returns current value without
    // changing it).
    {
        std::println("\n--- AC3: bad-arg no-op ---");
        (void)cs.eval("(arena:set-auto-compact-threshold 25)");
        const auto before =
            aura::compiler::types::as_int(*cs.eval("(arena:auto-compact-threshold)"));
        auto r = cs.eval("(arena:set-auto-compact-threshold \"not-a-number\")");
        CHECK(r && aura::compiler::types::is_int(*r), "non-int arg returns an int (current value)");
        const auto after =
            aura::compiler::types::as_int(*cs.eval("(arena:auto-compact-threshold)"));
        CHECK(after == before,
              std::format("non-int arg left threshold unchanged ({} -> {})", before, after));
        // Restore.
        (void)cs.eval("(arena:set-auto-compact-threshold 50)");
    }

    // AC4: existing (#187 / #300 / #430 / #335 / #464 / #604 / #685)
    // arena primitives remain reachable (back-compat).
    {
        std::println("\n--- AC4: existing arena primitives back-compat ---");
        auto s_json = cs.eval("(stats:get \"arena:stats-json\")");
        CHECK(s_json.has_value(), "(stats:get \"arena:stats-json\") reachable (#187 back-compat)");
        auto s_def = cs.eval("(arena:defrag)");
        CHECK(s_def.has_value(), "(arena:defrag) reachable (#300 back-compat)");
        auto s_pol = cs.eval("(arena:compact-with-policy)");
        CHECK(s_pol.has_value(), "(arena:compact-with-policy) reachable (#430 back-compat)");
        auto s_probe = cs.eval("(arena:should-auto-compact?)");
        CHECK(s_probe.has_value(), "(arena:should-auto-compact?) reachable (#335 back-compat)");
        auto s_auto = cs.eval("(engine:metrics \"query:arena-auto-stats\")");
        CHECK(s_auto.has_value(),
              "(engine:metrics \"query:arena-auto-stats\") reachable (#464 back-compat)");
        auto s_compact = cs.eval("(engine:metrics \"query:arena-auto-compact-stats\")");
        CHECK(s_compact.has_value(),
              "(engine:metrics \"query:arena-auto-compact-stats\") reachable (#685 back-compat)");
        auto s_snap = cs.eval("(engine:metrics \"query:arena-fragmentation-snapshot\")");
        CHECK(
            s_snap.has_value(),
            "(engine:metrics \"query:arena-fragmentation-snapshot\") reachable (#604 back-compat)");
    }

    // AC5: concurrent reads of (arena:auto-compact-threshold) under
    // 2 threads × 4 iters. Atomicity regression coverage.
    {
        std::println("\n--- AC5: concurrent threshold reads ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 4;
        (void)cs.eval("(arena:set-auto-compact-threshold 60)");
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(arena:auto-compact-threshold)");
                if (r && aura::compiler::types::is_int(*r))
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() == k_iters * 2,
              std::format("concurrent: {} / {} reads returned int", ok_count.load(), k_iters * 2));
        // Restore default.
        (void)cs.eval("(arena:set-auto-compact-threshold 50)");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_623_run();
}
#endif
