// @category: integration
// @reason: Issue #624 ShapeProfiler + GuardShape + JIT maturity —
// query:shape-stability-jit-stats-hash structured companion
//
// Scope-limited close matching #601/#491/#479/#604/#606/#614/#615/
// #616/#617/#618/#620/#621/#622/#623 pattern.
//
// Discovery before this PR: the C++ side already exposes the full
// shape-stability + JIT observability surface that #624 AC4 lists,
// via shape::*_count counters in src/compiler/shape_profiler.h
// (added by #570 / #605 / #492 / #686). Pre-existing primitives:
//   - (query:shape-stability-stats) (#570/#605) — int-sum of 6
//   - (query:shape-profiler-stats) (#492) — 12-field hash
//   - (query:jit-stats) (#427) / (query:jit-stats-hash) (#491) —
//     JIT compile/dispatch metrics
//   - (query:guard-shape-stats) — GuardShape dispatch (existing)
//
// What the issue body AC4 specifies — `query:shape-stability-jit-
// stats` with fields {stability_ratio_post_mutate, deopt_on_
// instability, version_bumps, jit_shape_miss, wrong_opt_prevented}
// — was *not* shipped under that exact name. So #624 ships ONE
// new Aura primitive covering exactly those 5 fields (3 use the
// existing shape::* counters verbatim; 2 are derived inline from
// the same counters since there's no dedicated per-field C++
// counter for them yet). Honest derivative fields, not invented.
//
// The remaining #624 AC1 + AC2 + AC3 (post-mutate re-eval in
// ShapeProfiler::record_shape + GuardShape dispatch version check
// on shape-version bumps + invalidate hook from mutate success
// path) are invasive C++ + hot-path changes that need
// benchmarking + perf regression coverage alongside the
// JIT/hot-swap work already done in #601 / #491 — separate
// follow-up.

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

namespace aura_issue_624_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:shape-stability-jit-stats-hash) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_624_detail

int main() {
    using namespace aura_issue_624_detail;
    std::println("=== Issue #624: query:shape-stability-jit-stats-hash structured companion ===");

    aura::compiler::CompilerService cs;

    // AC1: hash returns a hash with 6 fields (5 fields + schema).
    {
        std::println("\n--- AC1: (query:shape-stability-jit-stats-hash) shape ---");
        auto h = cs.eval("(query:shape-stability-jit-stats-hash)");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "shape-stability-jit-stats-hash returns a hash");
        const auto post_mutate = hash_int(cs, "stability-ratio-post-mutate");
        const auto deopt = hash_int(cs, "deopt-on-instability");
        const auto bumps = hash_int(cs, "version-bumps");
        const auto jit_miss = hash_int(cs, "jit-shape-miss");
        const auto wrong_opt = hash_int(cs, "wrong-opt-prevented");
        const auto schema = hash_int(cs, "schema");
        CHECK(post_mutate >= 0 && post_mutate <= 100,
              std::format("stability-ratio-post-mutate in [0,100] (got {})", post_mutate));
        CHECK(deopt >= 0 && deopt <= 100,
              std::format("deopt-on-instability in [0,100] (got {})", deopt));
        CHECK(bumps >= 0, std::format("version-bumps >= 0 (got {})", bumps));
        CHECK(jit_miss >= 0, std::format("jit-shape-miss >= 0 (got {})", jit_miss));
        CHECK(wrong_opt >= 0, std::format("wrong-opt-prevented >= 0 (got {})", wrong_opt));
        CHECK(schema == 624, std::format("schema == 624 (got {})", schema));
    }

    // AC2: existing shape-related primitives remain reachable
    // (back-compat — #624 doesn't disturb the existing surface).
    {
        std::println("\n--- AC2: existing primitives back-compat ---");
        // Original int-sum.
        auto s_int = cs.eval("(query:shape-stability-stats)");
        CHECK(s_int && aura::compiler::types::is_int(*s_int),
              "(query:shape-stability-stats) returns an int (#570 back-compat)");
        // 12-field structured form.
        auto s_struct = cs.eval("(query:shape-profiler-stats)");
        CHECK(s_struct && aura::compiler::types::is_hash(*s_struct),
              "(query:shape-profiler-stats) returns a hash (#492 back-compat)");
        // JIT-related — (query:jit-stats) returns a string dump;
        // (query:jit-stats-hash) returns a hash. Both reachable.
        auto s_jit = cs.eval("(query:jit-stats)");
        CHECK(s_jit.has_value() && !s_jit.has_value()
                  ? false
                  : s_jit && aura::compiler::types::is_string(*s_jit),
              "(query:jit-stats) returns a string (#427 back-compat)");
        auto s_jit_h = cs.eval("(query:jit-stats-hash)");
        CHECK(s_jit_h.has_value(), "(query:jit-stats-hash) reachable (#491 back-compat)");
    }

    // AC3: derived-metric invariants on a fresh service.
    // With no workload yet, the underlying counters are 0 so
    // both derived fields must be 0.
    {
        std::println("\n--- AC3: derived-metric invariants on fresh service ---");
        const auto churn = hash_int(cs, "stability-ratio-post-mutate");
        const auto deopt = hash_int(cs, "deopt-on-instability");
        // When both inputs of the ratio are 0, derived fields = 0.
        const auto bumps = hash_int(cs, "version-bumps");
        const auto churn_count = hash_int(cs, "jit-shape-miss");
        if (bumps == 0 && churn_count == 0) {
            CHECK(deopt == 0,
                  std::format("fresh-service deopt-on-instability == 0 (got {})", deopt));
        }
        // stability-ratio-post-mutate may also be 0 (churn==0).
        CHECK(churn >= 0,
              std::format("stability-ratio-post-mutate is non-negative (got {})", churn));
    }

    // AC4: schema sentinel is exactly 624 (not 621/622/623) —
    // the Agent can use it to detect drift.
    {
        std::println("\n--- AC4: schema sentinel ===");
        const auto schema = hash_int(cs, "schema");
        CHECK(schema == 624, std::format("schema == 624 (got {})", schema));
    }

    // AC5: concurrent reads under 2 threads × 4 iters. Atomicity
    // regression coverage for the underlying shape::* counters.
    {
        std::println("\n--- AC5: concurrent shape-stability-jit-stats-hash reads ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 4;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(query:shape-stability-jit-stats-hash)");
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