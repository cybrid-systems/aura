// @category: integration
// @reason: Issue #684 IRSoA full wiring + incremental mutate

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_684_detail {
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

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:irsoa-incremental-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_684_detail

int aura_issue_684_run() {
    using namespace aura_issue_684_detail;
    std::println("=== Issue #684: IRSoA full wiring incremental ===");

    aura::compiler::CompilerService cs;
    // Issue #1377: dual-emit is opt-in; this test verifies the #684
    // wiring surface and must enable it explicitly.
    cs.set_soa_dual_emit(true);

    // AC1: stats hash fields
    {
        std::println("\n--- AC1: query:irsoa-incremental-stats ---");
        auto stats = cs.eval("(query:irsoa-incremental-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:irsoa-incremental-stats returns hash");
        CHECK(stat_int(cs, "soa-wired-hits") >= 0, "soa-wired-hits present");
        CHECK(stat_int(cs, "dirty-cascade-savings") >= 0, "dirty-cascade-savings present");
        CHECK(stat_int(cs, "partial-relower-ratio") >= 0, "partial-relower-ratio present");
        CHECK(stat_int(cs, "cache-miss-reduction") >= 0, "cache-miss-reduction present");
    }

    const auto wired_before = stat_int(cs, "soa-wired-hits");

    // AC2: recursive fact + dual-emit wiring
    {
        std::println("\n--- AC2: fact eval + SoA dual-emit ---");
        cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))\" )");
        cs.eval("(eval-current)");
        for (int i = 0; i < 15; ++i)
            (void)cs.eval("(fact 4)");
        const auto wired_after = stat_int(cs, "soa-wired-hits");
        CHECK(wired_after > wired_before,
              std::format("soa-wired-hits grew ({} -> {})", wired_before, wired_after));
    }

    const auto cascade_before = stat_int(cs, "dirty-cascade-savings");
    const auto ratio_before = stat_int(cs, "partial-relower-ratio");

    // AC3: mutate:rebind + incremental re-lower metrics
    {
        std::println("\n--- AC3: mutate:rebind + incremental metrics ---");
        auto r = cs.eval("(mutate:rebind \"fact\" "
                         "\"(lambda (n) (if (= n 0) 1 (* n (fact (- n 1)))))\" "
                         "\"issue-684\")");
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "mutate:rebind on fact succeeds");
        cs.eval("(eval-current)");
        auto fact5 = cs.eval("(fact 5)");
        CHECK(fact5 && aura::compiler::types::is_int(*fact5) &&
                  aura::compiler::types::as_int(*fact5) == 120,
              "fact 5 == 120 after rebind + eval-current");
        const auto cascade_after = stat_int(cs, "dirty-cascade-savings");
        CHECK(cascade_after >= cascade_before,
              std::format("dirty-cascade-savings non-decreasing ({} -> {})", cascade_before,
                          cascade_after));
    }

    // AC4: repeated clean re-lower skips for partial ratio + fiber stress
    {
        std::println("\n--- AC4: partial re-lower ratio + fiber stress ---");
        auto* flat = cs.evaluator().workspace_flat();
        auto* pool = cs.evaluator().workspace_pool();
        CHECK(flat != nullptr && pool != nullptr, "workspace flat/pool available");
        if (flat && pool) {
            for (int i = 0; i < 12; ++i) {
                (void)cs.relower_define_blocks(
                    "fact", "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))", *flat, *pool,
                    flat->root);
            }
        }
        const auto ratio_after = stat_int(cs, "partial-relower-ratio");
        CHECK(ratio_after >= ratio_before,
              std::format("partial-relower-ratio non-decreasing ({} -> {})", ratio_before,
                          ratio_after));
        CHECK(ratio_after >= 70,
              std::format("partial-relower-ratio >= 70% (got {}%)", ratio_after));

        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 25;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(eval-current)");
                auto r = cs.eval("(fact 3)");
                if (r && aura::compiler::types::is_int(*r) &&
                    aura::compiler::types::as_int(*r) == 6)
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() == k_iters * 2,
              std::format("fiber stress: {} / {} correct", ok_count.load(), k_iters * 2));
    }

    // AC5: stats registry
    {
        std::println("\n--- AC5: stats:list + stats:count ---");
        auto r = cs.eval("(stats:count)");
        const auto n =
            r && aura::compiler::types::is_int(*r) ? aura::compiler::types::as_int(*r) : 0;
        CHECK(n >= 64, std::format("stats:count >= 64 (got {})", n));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_684_run();
}
#endif
