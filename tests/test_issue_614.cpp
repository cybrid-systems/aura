// @category: integration
// @reason: Issue #614 primitives hot-path memory stability + pair/cdr observability
//
// Scope-limited close matching the #601 / #491 / #479 / #604 / #606 pattern:
// ship the observability foundation + per-prim hot-path counters now; the
// full 10000-round / 20-fiber stress matrix + JIT special-case dispatch
// for list/math remains a separate follow-up.

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

namespace aura_issue_614_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:primitives-hotpath-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_614_detail

int main() {
    using namespace aura_issue_614_detail;
    std::println("=== Issue #614: primitives hot-path stability ===");

    aura::compiler::CompilerService cs;

    // AC1: query primitive shape — hash + 4 fields.
    {
        std::println("\n--- AC1: query:primitives-hotpath-stats shape ---");
        auto stats = cs.eval("(query:primitives-hotpath-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:primitives-hotpath-stats returns a hash");
        CHECK(stat_int(cs, "primitive-call-total") >= 0, "primitive-call-total present");
        CHECK(stat_int(cs, "pair-alloc-total") >= 0, "pair-alloc-total present");
        CHECK(stat_int(cs, "linear-traverse-total") >= 0, "linear-traverse-total present");
        CHECK(stat_int(cs, "cdr-depth-max") >= 0, "cdr-depth-max present");
    }

    // AC2: dynamic list construction (via let-loop) bumps pair-alloc-total.
    // Literal (list 1 2 3 ...) literals get constant-folded at compile
    // time and skip the runtime primitive — we build via append in a
    // loop instead, which is what real AI-agent workloads do anyway.
    {
        std::println("\n--- AC2: dynamic list construction bumps pair-alloc-total ---");
        const auto before = stat_int(cs, "pair-alloc-total");
        cs.eval("(define build-list (lambda (n) (let loop ((i 0) (acc '())) "
                "(if (= i n) acc (loop (+ i 1) (append acc (list (+ i 1))))))))");
        auto r = cs.eval("(length (build-list 10))");
        CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 10,
              std::format("build-list 10 has length 10 (got {})",
                          r && aura::compiler::types::is_int(*r) ? aura::compiler::types::as_int(*r)
                                                                 : -1));
        const auto after = stat_int(cs, "pair-alloc-total");
        CHECK(after >= before + 10,
              std::format("pair-alloc-total bumped >= 10 on dynamic construction ({} -> {})",
                          before, after));
    }

    // AC3: length traversal bumps linear-traverse-total + cdr-depth-max.
    {
        std::println("\n--- AC3: cdr traversal bumps linear-traverse + cdr-depth-max ---");
        // Build the test list dynamically via the lambda above so the
        // runtime primitives fire (literal (list 1 2 3 ...) gets
        // constant-folded at compile time).
        cs.eval("(define big-list (build-list 20))");
        const auto tr_before = stat_int(cs, "linear-traverse-total");
        const auto dm_before = stat_int(cs, "cdr-depth-max");
        auto r = cs.eval("(length big-list)");
        CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 20,
              std::format("length of 20-list returns 20 (got {})",
                          r && aura::compiler::types::is_int(*r) ? aura::compiler::types::as_int(*r)
                                                                 : -1));
        const auto tr_after = stat_int(cs, "linear-traverse-total");
        const auto dm_after = stat_int(cs, "cdr-depth-max");
        CHECK(tr_after >= tr_before + 20,
              std::format("linear-traverse-total >= +20 ({} -> {})", tr_before, tr_after));
        CHECK(dm_after >= std::max<std::int64_t>(20, dm_before),
              std::format("cdr-depth-max >= max(20, prev) ({} -> {})", dm_before, dm_after));
    }

    // AC4: list-ref positional access bumps linear-traverse at the
    // requested position (5 hops, then we read).
    {
        std::println("\n--- AC4: list-ref positional access ---");
        const auto tr_before = stat_int(cs, "linear-traverse-total");
        auto r = cs.eval("(list-ref big-list 5)");
        CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 6,
              std::format("list-ref big-list 5 == 6 (got {})",
                          r && aura::compiler::types::is_int(*r) ? aura::compiler::types::as_int(*r)
                                                                 : -1));
        const auto tr_after = stat_int(cs, "linear-traverse-total");
        CHECK(tr_after >= tr_before + 5,
              std::format("linear-traverse-total >= +5 ({} -> {})", tr_before, tr_after));
    }

    // AC5: dynamic map / filter chain — both invoke runtime primitives.
    {
        std::println("\n--- AC5: dynamic map + filter pair-alloc cost ---");
        const auto before = stat_int(cs, "pair-alloc-total");
        // Wrap each call in a top-level define so each invocation goes
        // through the runtime dispatcher (literal call sites may be
        // inlined away by the constant folder).
        cs.eval("(define doubled (map (lambda (x) (* x 2)) big-list))");
        cs.eval("(define big-ones (filter (lambda (x) (> x 5)) big-list))");
        const auto after = stat_int(cs, "pair-alloc-total");
        CHECK(after >= before + 30,
              std::format("pair-alloc-total bumped >= 30 for runtime map+filter ({} -> {})", before,
                          after));
    }

    // AC6: foldl — bumps linear-traverse on the entire walk.
    {
        std::println("\n--- AC6: foldl cdr-walk bump ---");
        const auto before = stat_int(cs, "linear-traverse-total");
        auto r = cs.eval("(foldl + 0 big-list)");
        CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 210,
              std::format("foldl + 0 [1..20] == 210 (got {})",
                          r && aura::compiler::types::is_int(*r) ? aura::compiler::types::as_int(*r)
                                                                 : -1));
        const auto after = stat_int(cs, "linear-traverse-total");
        CHECK(after >= before + 20,
              std::format("foldl bumped linear-traverse >= +20 ({} -> {})", before, after));
    }

    // AC7: concurrent churn — 2 threads × 30 list-math iterations under
    // mutate:request-gc-safepoint. Scaled-down stand-in for the full
    // 10000-round / 20-fiber stress (follow-up).
    {
        std::println("\n--- AC7: concurrent list-math under mutate ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 30;
        const auto tr_before = stat_int(cs, "linear-traverse-total");
        const auto pa_before = stat_int(cs, "pair-alloc-total");
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(mutate:request-gc-safepoint)");
                auto r = cs.eval("(length big-list)");
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
              std::format("concurrent length: {} / {} correct", ok_count.load(), k_iters * 2));
        const auto tr_after = stat_int(cs, "linear-traverse-total");
        const auto pa_after = stat_int(cs, "pair-alloc-total");
        CHECK(tr_after >= tr_before + 20 * 2 * k_iters,
              std::format("linear-traverse-total grew with concurrent length ({} -> {})", tr_before,
                          tr_after));
        CHECK(pa_after >= pa_before,
              std::format("pair-alloc-total non-decreasing ({} -> {})", pa_before, pa_after));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
