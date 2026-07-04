// @category: integration
// @reason: Issue #683 linear ownership + GC safepoint / fiber-steal integration

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_683_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:linear-ownership-gc-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_683_detail

int main() {
    using namespace aura_issue_683_detail;
    std::println("=== Issue #683: linear ownership GC safepoint / steal ===");

    aura::compiler::CompilerService cs;

    // AC1: stats hash fields
    {
        std::println("\n--- AC1: query:linear-ownership-gc-stats ---");
        auto stats = cs.eval("(query:linear-ownership-gc-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:linear-ownership-gc-stats returns hash");
        CHECK(stat_int(cs, "safepoint-violations") >= 0, "safepoint-violations present");
        CHECK(stat_int(cs, "steal-enforced") >= 0, "steal-enforced present");
        CHECK(stat_int(cs, "relower-revalidate-hits") >= 0, "relower-revalidate-hits present");
    }

    // AC2: recursive fact + GC safepoint linear probe
    {
        std::println("\n--- AC2: fact eval + mutate:request-gc-safepoint ---");
        cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))\" )");
        cs.eval("(eval-current)");
        for (int i = 0; i < 20; ++i)
            (void)cs.eval("(fact 4)");
        const auto pass_before = cs.get_linear_check_pass_count();
        const auto safepoint_before = stat_int(cs, "safepoint-violations");
        auto sp = cs.eval("(mutate:request-gc-safepoint)");
        CHECK(sp && aura::compiler::types::is_int(*sp), "mutate:request-gc-safepoint returns int");
        const auto pass_after = cs.get_linear_check_pass_count();
        CHECK(pass_after >= pass_before,
              std::format("linear_check_pass non-decreasing ({} -> {})", pass_before, pass_after));
        const auto safepoint_after = stat_int(cs, "safepoint-violations");
        CHECK(safepoint_after >= safepoint_before,
              std::format("safepoint-violations non-decreasing ({} -> {})", safepoint_before,
                          safepoint_after));
    }

    const auto relower_before = stat_int(cs, "relower-revalidate-hits");
    const auto steal_before = stat_int(cs, "steal-enforced");

    // AC3: mutate:rebind triggers re-lower revalidate probe
    {
        std::println("\n--- AC3: mutate:rebind + relower revalidate ---");
        auto r = cs.eval("(mutate:rebind \"fact\" "
                         "\"(lambda (n) (if (= n 0) 1 (* n (fact (- n 1)))))\" "
                         "\"issue-683\")");
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "mutate:rebind on fact succeeds");
        cs.eval("(eval-current)");
        auto fact5 = cs.eval("(fact 5)");
        CHECK(fact5 && aura::compiler::types::is_int(*fact5) &&
                  aura::compiler::types::as_int(*fact5) == 120,
              "fact 5 == 120 after rebind + eval-current");
        const auto relower_after = stat_int(cs, "relower-revalidate-hits");
        CHECK(relower_after > relower_before, std::format("relower-revalidate-hits grew ({} -> {})",
                                                          relower_before, relower_after));
    }

    // AC4: fiber steal probe + stress path
    {
        std::println("\n--- AC4: fiber steal probe + stress ---");
        cs.evaluator().transfer_mutation_stack_to_current_fiber();
        const auto steal_after_probe = stat_int(cs, "steal-enforced");
        CHECK(steal_after_probe > steal_before,
              std::format("steal-enforced grew after transfer ({} -> {})", steal_before,
                          steal_after_probe));

        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 30;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(mutate:request-gc-safepoint)");
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
        CHECK(n >= 62, std::format("stats:count >= 62 (got {})", n));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}