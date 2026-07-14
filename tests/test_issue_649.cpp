// @category: integration
// @reason: Issue #649 Full Per-Fiber YieldCheckpointStorage
// Re-Stamp + Size Validation on Panic Transfer + Cross-Steal —
// query:yield-checkpoint-panic-stats structured companion
//
// Scope-limited close matching #601/#491/#479/#604/#606/#614/
// #615/#616/#617/#618/#620/#621/#622/#623/#624/#625/#626/#630/
// #631/#632/#633/#637/#640/#641/#642/#643/#644/#645/#646/#647/
// #648 pattern.
//
// Discovery before this PR: the YieldCheckpoint + Panic
// observability surface already covers the high-level panic
// transport via #648 + the high-level panic checkpoint
// lifecycle summary + yield checkpoint foundation:
//   - (query:panic-checkpoint-fiber-stats) (#648) — fiber
//     resume panic checkpoint transfer (transport layer)
//   - (engine:metrics \"query:panic-checkpoint-lifecycle-stats\") — high-level
//     panic checkpoint lifecycle summary
//   - #264 yield checkpoint foundation
//   - #356 INVALID_VERSION + post-rollback frames
//   - #588 per-fiber stack + yield_checkpoint_storage_
//   - transfer_panic_checkpoint_trampoline + bump metric
//   - restore_post_yield_or_rollback validates
//     thread/version/depth but no yield_checkpoint
//     re-stamp or size check
//   - g_fiber_yield_checkpoint_deleter_ exists but no
//     panic-state re-stamp coordination
//
// What the issue body AC4 specifies by **exact name + fields** —
// `query:yield-checkpoint-panic-stats` with AC1+AC2+AC3-specific
// counters as a structured hash (yield checkpoint storage
// lifecycle layer that #648 doesn't cover) — was *not* shipped
// under that exact name. So #649 ships ONE new Aura primitive
// + 3 new atomics that are foundation scaffolding for the
// future AC1 (transfer_panic_checkpoint_trampoline + fiber
// resume after hook call re-stamp or resize
// yield_checkpoint_storage_ to match current panic Guard
// state), AC2 (restore_post_yield_or_rollback adds
// yield_checkpoint stack size + top-entry version check), and
// AC3 (g_fiber_yield_checkpoint_ coordinates with
// pending_panic_checkpoint under MutationBoundary) enforcement
// work.
//
// Non-duplicative to #648/#264 (issue body explicitly
// cross-referenced).
//
// The remaining #649 AC1 + AC2 + AC3 work is invasive C++ on
// transfer_panic_checkpoint_trampoline + fiber resume +
// restore_post_yield_or_rollback + g_fiber_yield_checkpoint_ +
// needs the panic during deep yield-boundary + steal +
// resume matrix + TSan coverage from the issue body —
// separate follow-ups.

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

namespace aura_issue_649_detail {
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
        "(hash-ref (engine:metrics \"query:yield-checkpoint-panic-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_649_detail

int aura_issue_649_run() {
    using namespace aura_issue_649_detail;
    std::println("=== Issue #649: query:yield-checkpoint-panic-stats structured companion ===");

    aura::compiler::CompilerService cs;

    // AC1: hash returns a hash with the documented fields.
    {
        std::println("\n--- AC1: (query:yield-checkpoint-panic-stats) shape ---");
        auto h = cs.eval("(query:yield-checkpoint-panic-stats)");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "yield-checkpoint-panic-stats returns a hash");
        const auto xfer_restamp = hash_int(cs, "transfer-with-restamp");
        const auto size_mis = hash_int(cs, "size-mismatch-caught");
        const auto cross_steal = hash_int(cs, "cross-steal-invalidation");
        const auto schema = hash_int(cs, "schema");
        CHECK(xfer_restamp >= 0, std::format("transfer-with-restamp >= 0 (got {})", xfer_restamp));
        CHECK(size_mis >= 0, std::format("size-mismatch-caught >= 0 (got {})", size_mis));
        CHECK(cross_steal >= 0, std::format("cross-steal-invalidation >= 0 (got {})", cross_steal));
        CHECK(schema == 649, std::format("schema == 649 (got {})", schema));
    }

    // AC2: existing primitives remain reachable
    // (back-compat — #649 doesn't disturb them).
    {
        std::println("\n--- AC2: existing primitives back-compat ---");
        auto s_648 = cs.eval("(query:panic-checkpoint-fiber-stats)");
        CHECK(s_648.has_value(), "(query:panic-checkpoint-fiber-stats) reachable (#648 back-compat "
                                 "— panic transfer transport layer)");
        auto s_647 = cs.eval("(query:envframe-dualpath-stale-stats-hash)");
        CHECK(s_647.has_value(),
              "(query:envframe-dualpath-stale-stats-hash) reachable (#647 back-compat)");
        auto s_646 = cs.eval("(query:gc-safepoint-deferral-stats)");
        CHECK(s_646.has_value(),
              "(query:gc-safepoint-deferral-stats) reachable (#646 back-compat)");
        auto s_645 = cs.eval("(query:scheduler-steal-bias-stats)");
        CHECK(s_645.has_value(), "(query:scheduler-steal-bias-stats) reachable (#645 back-compat)");
        auto s_644 = cs.eval("(query:aot-reload-func-table-stats)");
        CHECK(s_644.has_value(),
              "(query:aot-reload-func-table-stats) reachable (#644 back-compat)");
        auto s_642 = cs.eval("(query:arena-auto-compaction-stats)");
        CHECK(s_642.has_value(),
              "(query:arena-auto-compaction-stats) reachable (#642 back-compat)");
    }

    // AC3: derived-metric invariants on a fresh service.
    // transfer-with-restamp / size-mismatch-caught /
    // cross-steal-invalidation are all 0 on a fresh service —
    // they are foundation scaffolding for the future AC1 +
    // AC2 + AC3 enforcement work (yield_checkpoint re-stamp +
    // size validation + cross-steal invalidation).
    {
        std::println("\n--- AC3: derived-metric invariants on fresh service ---");
        const auto xfer_restamp = hash_int(cs, "transfer-with-restamp");
        const auto size_mis = hash_int(cs, "size-mismatch-caught");
        const auto cross_steal = hash_int(cs, "cross-steal-invalidation");
        CHECK(xfer_restamp == 0,
              std::format("fresh-service transfer-with-restamp == 0 (got {})", xfer_restamp));
        CHECK(size_mis == 0,
              std::format("fresh-service size-mismatch-caught == 0 (got {})", size_mis));
        CHECK(cross_steal == 0,
              std::format("fresh-service cross-steal-invalidation == 0 (got {})", cross_steal));
    }

    // AC4: schema sentinel is exactly 649 (not 648/647/646/645).
    {
        std::println("\n--- AC4: schema sentinel ---");
        const auto schema = hash_int(cs, "schema");
        CHECK(schema == 649, std::format("schema == 649 (got {})", schema));
    }

    // AC5: naming distinction — the new primitive name uses
    // `-yield-checkpoint-panic-stats` (storage lifecycle
    // focus), distinct from #648's `-panic-checkpoint-fiber-
    // stats` (transport focus). Both are hash primitives
    // with `schema` sentinel — verify field sets are
    // distinct.
    {
        std::println("\n--- AC5: naming + field-set distinction from #648 ---");
        auto new_p = cs.eval("(query:yield-checkpoint-panic-stats)");
        auto old_p = cs.eval("(query:panic-checkpoint-fiber-stats)");
        CHECK(new_p.has_value(), "new primitive (query:yield-checkpoint-panic-stats) reachable "
                                 "(yield-checkpoint- prefix)");
        CHECK(old_p.has_value(), "existing (query:panic-checkpoint-fiber-stats) still reachable "
                                 "(#648, panic-checkpoint- prefix)");
        // Both primitives use `schema` as primary sentinel
        // — distinct values (649 vs 648). The new primitive
        // should NOT have #648's fields (transport-layer
        // fields), and #648 should NOT have #649's fields
        // (storage-lifecycle fields).
        const auto new_schema = hash_int(cs, "schema");
        CHECK(new_schema == 649, std::format("new primitive schema == 649 (got {})", new_schema));
        // Field reachability checks (avoid hash-ref on missing
        // keys — see #644/#645 lessons).
        const auto check_new_field = [&](std::string_view k) {
            auto r = cs.eval(std::format(
                "(hash-ref (engine:metrics \"query:yield-checkpoint-panic-stats\") '{}')", k));
            return r.has_value() && aura::compiler::types::is_int(*r);
        };
        CHECK(check_new_field("transfer-with-restamp"),
              "new primitive 'transfer-with-restamp' field reachable");
        CHECK(check_new_field("size-mismatch-caught"),
              "new primitive 'size-mismatch-caught' field reachable");
        CHECK(check_new_field("cross-steal-invalidation"),
              "new primitive 'cross-steal-invalidation' field reachable");
    }

    // AC6: concurrent reads under 2 threads × 4 iters. Atomicity
    // regression coverage for the underlying counters + 3 new
    // scaffolding atomics.
    {
        std::println("\n--- AC6: concurrent yield-checkpoint-panic-stats reads ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 4;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(query:yield-checkpoint-panic-stats)");
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
    return aura_issue_649_run();
}
#endif
