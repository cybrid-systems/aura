// @category: integration
// @reason: Issue #681 IRClosure/EnvFrame epoch enforcement post-mutate

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_681_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:compiler-closure-inval-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_681_detail

int aura_issue_681_run() {
    using namespace aura_issue_681_detail;
    std::println("=== Issue #681: compiler closure epoch enforcement ===");

    aura::compiler::CompilerService cs;

    // AC1: stats hash fields
    {
        std::println("\n--- AC1: query:compiler-closure-inval-stats ---");
        auto stats = cs.eval("(query:compiler-closure-inval-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:compiler-closure-inval-stats returns hash");
        CHECK(stat_int(cs, "stale-bridge-caught") >= 0, "stale-bridge-caught present");
        CHECK(stat_int(cs, "epoch-mismatch-hits") >= 0, "epoch-mismatch-hits present");
        CHECK(stat_int(cs, "safe-fallbacks") >= 0, "safe-fallbacks present");
    }

    // AC2: recursive fact + long-lived closure calls
    {
        std::println("\n--- AC2: recursive fact eval + closure capture ---");
        cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))\" )");
        cs.eval("(eval-current)");
        for (int i = 0; i < 30; ++i)
            (void)cs.eval("(fact 4)");
        auto fact5 = cs.eval("(fact 5)");
        CHECK(fact5 && aura::compiler::types::is_int(*fact5) &&
                  aura::compiler::types::as_int(*fact5) == 120,
              "fact 5 == 120 before mutate");
    }

    const auto bridge_inval_before = cs.get_compiler_inval_bridge_epoch_total();
    const auto mismatch_before = cs.get_compiler_closure_epoch_mismatch_hits();

    // AC3: mutate:rebind triggers invalidate + bridge expiry
    {
        std::println("\n--- AC3: mutate:rebind + post-inval re-eval ---");
        auto r = cs.eval("(mutate:rebind \"fact\" "
                         "\"(lambda (n) (if (= n 0) 1 (* n (fact (- n 1)))))\" "
                         "\"issue-681\")");
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "mutate:rebind on fact succeeds");
        cs.eval("(eval-current)");
        auto fact5 = cs.eval("(fact 5)");
        CHECK(fact5 && aura::compiler::types::is_int(*fact5) &&
                  aura::compiler::types::as_int(*fact5) == 120,
              "fact 5 == 120 after rebind + eval-current");
        const auto bridge_inval_after = cs.get_compiler_inval_bridge_epoch_total();
        CHECK(bridge_inval_after > bridge_inval_before,
              std::format("compiler_inval_bridge_epoch grew ({} -> {})", bridge_inval_before,
                          bridge_inval_after));
    }

    // AC4: metrics observable via stats primitive
    {
        std::println("\n--- AC4: closure inval metrics after mutate ---");
        const auto mismatch_after = stat_int(cs, "epoch-mismatch-hits");
        const auto safe_fb = stat_int(cs, "safe-fallbacks");
        CHECK(mismatch_after >= mismatch_before,
              std::format("epoch-mismatch-hits non-decreasing ({} -> {})", mismatch_before,
                          mismatch_after));
        CHECK(safe_fb >= 0, "safe-fallbacks readable");
        std::println("  epoch-mismatch={} safe-fallbacks={}", mismatch_after, safe_fb);
    }

    // AC5: fiber stress — closure calls stay correct post-mutate
    {
        std::println("\n--- AC5: fiber stress post-mutate ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 40;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
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

    // AC6: stats registry
    {
        std::println("\n--- AC6: stats:list + stats:count ---");
        auto r = cs.eval("(stats:count)");
        const auto n =
            r && aura::compiler::types::is_int(*r) ? aura::compiler::types::as_int(*r) : 0;
        CHECK(n >= 60, std::format("stats:count >= 60 (got {})", n));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_681_run();
}
#endif
