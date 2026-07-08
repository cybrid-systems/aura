// @category: integration
// @reason: Issue #693 Hardware backend SV commercial closed-loop

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.hardware_backend;

namespace aura_issue_693_detail {
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
    auto r =
        cs.eval(std::format("(hash-ref (query:hardware-backend-sv-closedloop-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_693_detail

int main() {
    using namespace aura_issue_693_detail;
    std::println("=== Issue #693: SV commercial feedback closed-loop ===");

    aura::compiler::CompilerService cs;

    // AC1: stats hash fields
    {
        std::println("\n--- AC1: query:hardware-backend-sv-closedloop-stats ---");
        auto stats = cs.eval("(query:hardware-backend-sv-closedloop-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:hardware-backend-sv-closedloop-stats returns hash");
        CHECK(stat_int(cs, "hook-calls") >= 0, "hook-calls present");
        CHECK(stat_int(cs, "commercial-reemits") >= 0, "commercial-reemits present");
        CHECK(stat_int(cs, "feedback-mutate-hits") >= 0, "feedback-mutate-hits present");
        CHECK(stat_int(cs, "ppa-savings") >= 0, "ppa-savings present");
        CHECK(stat_int(cs, "verification-loop-success") >= 0, "verification-loop-success present");
    }

    const auto hook_before = stat_int(cs, "hook-calls");
    const auto reemit_before = stat_int(cs, "commercial-reemits");
    const auto feedback_before = stat_int(cs, "feedback-mutate-hits");
    const auto loop_before = stat_int(cs, "verification-loop-success");

    // AC2: SV mutate triggers hardware hook + re-emit
    {
        std::println("\n--- AC2: mutate:sv-add-coverpoint closed-loop ---");
        cs.eval("(set-code \"(define cg 1)\")");
        cs.eval("(eval-current)");
        auto r = cs.eval("(mutate:sv-add-coverpoint 0 \"my_cp\")");
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "mutate:sv-add-coverpoint succeeds");
        const auto hook_after = stat_int(cs, "hook-calls");
        const auto reemit_after = stat_int(cs, "commercial-reemits");
        CHECK(hook_after > hook_before,
              std::format("hook-calls grew ({} -> {})", hook_before, hook_after));
        CHECK(reemit_after > reemit_before,
              std::format("commercial-reemits grew ({} -> {})", reemit_before, reemit_after));
    }

    // AC3: eda:run-verification-feedback closed-loop
    {
        std::println("\n--- AC3: eda:run-verification-feedback ---");
        auto r = cs.eval("(eda:run-verification-feedback \"coverage.log\" \"0 hole_a\")");
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "eda:run-verification-feedback coverage succeeds");
        auto r2 = cs.eval("(eda:run-verification-feedback \"assert-fail.log\" \"0 fail_msg\")");
        CHECK(r2 && aura::compiler::types::is_bool(*r2) && aura::compiler::types::as_bool(*r2),
              "eda:run-verification-feedback assert-fail succeeds");
        const auto feedback_after = stat_int(cs, "feedback-mutate-hits");
        const auto loop_after = stat_int(cs, "verification-loop-success");
        CHECK(feedback_after > feedback_before,
              std::format("feedback-mutate-hits grew ({} -> {})", feedback_before, feedback_after));
        CHECK(loop_after > loop_before,
              std::format("verification-loop-success grew ({} -> {})", loop_before, loop_after));
    }

    // AC4: stats:count
    {
        std::println("\n--- AC4: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 211,
              "stats:count >= 211");
    }

    // AC5: fiber stress
    {
        std::println("\n--- AC5: fiber stress ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 20;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(typecheck-current)");
                auto r = cs.eval("(eda:run-verification-feedback \"coverage.log\" \"0 stress\")");
                if (r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r))
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() > 0,
              std::format("fiber stress produced {} successful feedback loops", ok_count.load()));
    }

    aura::compiler::hardware::clear_structural_mutation_hook();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}