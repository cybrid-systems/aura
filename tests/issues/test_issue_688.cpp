// @category: integration
// @reason: Issue #688 linear OwnershipEnv post-mutate typed-mutation wiring

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_688_detail {
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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:linear-ownership-typed-mutate-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static constexpr const char* k_linear_prog = R"(
(define leak (let ((x (Linear 42))) (display x)))
(define f (lambda (n)
  (if (= n 0)
      (let ((l (Linear 1))) (move l))
      (let ((l2 (Linear 2))) (move l2)))))
)";

} // namespace aura_issue_688_detail

int aura_issue_688_run() {
    using namespace aura_issue_688_detail;
    std::println("=== Issue #688: linear ownership typed-mutate ===");

    aura::compiler::CompilerService cs;

    // AC1: stats hash fields
    {
        std::println("\n--- AC1: query:linear-ownership-typed-mutate-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:linear-ownership-typed-mutate-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:linear-ownership-typed-mutate-stats returns hash");
        CHECK(stat_int(cs, "post-mutate-revalidates") >= 0, "post-mutate-revalidates present");
        CHECK(stat_int(cs, "violations-caught") >= 0, "violations-caught present");
        CHECK(stat_int(cs, "enforcement-hits") >= 0, "enforcement-hits present");
        CHECK(stat_int(cs, "safe-fallbacks") >= 0, "safe-fallbacks present");
    }

    const auto reval_before = stat_int(cs, "post-mutate-revalidates");
    const auto enforce_before = stat_int(cs, "enforcement-hits");

    // AC2: linear if/predicate + eval + mutate:rebind
    {
        std::println("\n--- AC2: linear if/predicate + mutate:rebind ---");
        cs.eval(std::format("(set-code \"{}\")", k_linear_prog));
        cs.eval("(eval-current)");
        (void)cs.eval("(f 0)");
        (void)cs.eval("(f 1)");
        auto r = cs.eval("(mutate:rebind \"f\" "
                         "\"(lambda (n) (if (= n 0) "
                         "(let ((l (Linear 10))) (move l)) "
                         "(let ((l2 (Linear 20))) (move l2))))\" "
                         "\"issue-688\")");
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "mutate:rebind on linear f succeeds");
        cs.eval("(eval-current)");
        auto f0 = cs.eval("(f 0)");
        CHECK(f0 && aura::compiler::types::is_int(*f0) && aura::compiler::types::as_int(*f0) == 10,
              "f 0 == 10 after rebind");
        const auto reval_after = stat_int(cs, "post-mutate-revalidates");
        const auto enforce_after = stat_int(cs, "enforcement-hits");
        CHECK(reval_after > reval_before,
              std::format("post-mutate-revalidates grew ({} -> {})", reval_before, reval_after));
        CHECK(enforce_after >= enforce_before,
              std::format("enforcement-hits non-decreasing ({} -> {})", enforce_before,
                          enforce_after));
    }

    // AC3: GC safepoint + linear probe path
    {
        std::println("\n--- AC3: GC safepoint linear probe ---");
        const auto fallback_before = stat_int(cs, "safe-fallbacks");
        (void)cs.eval("(mutate:request-gc-safepoint)");
        const auto fallback_after = stat_int(cs, "safe-fallbacks");
        CHECK(fallback_after >= fallback_before,
              std::format("safe-fallbacks non-decreasing ({} -> {})", fallback_before,
                          fallback_after));
    }

    // AC4: fiber stress
    {
        std::println("\n--- AC4: fiber stress ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 20;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(mutate:request-gc-safepoint)");
                auto r = cs.eval("(f 1)");
                if (r && aura::compiler::types::is_int(*r) &&
                    aura::compiler::types::as_int(*r) == 20)
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
        CHECK(n >= 68, std::format("stats:count >= 68 (got {})", n));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_688_run();
}
#endif
