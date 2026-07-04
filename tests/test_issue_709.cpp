// @category: integration
// @reason: Issue #709 primitives registry fast dispatch + capture discipline + EDA integration

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_709_detail {
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

static std::int64_t reg_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:primitives-registry-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::int64_t ext_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:primitives-extension-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_709_detail

int main() {
    using namespace aura_issue_709_detail;

    std::println("=== Issue #709: Primitives registry fastpath + capture discipline ===");

    aura::compiler::CompilerService cs;

    // AC1: query:primitives-registry-stats hash fields
    {
        std::println("\n--- AC1: query:primitives-registry-stats ---");
        auto stats = cs.eval("(query:primitives-registry-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:primitives-registry-stats returns hash");
        CHECK(reg_stat(cs, "capture-violations") >= 0, "capture-violations present");
        CHECK(reg_stat(cs, "fastpath-hits") >= 0, "fastpath-hits present");
        CHECK(reg_stat(cs, "eda-registered") >= 4, "eda-registered >= 4 SV/EDA primitives");
        CHECK(reg_stat(cs, "consistency-rate") >= 0, "consistency-rate present");
        CHECK(reg_stat(cs, "registry-slots") > 0, "registry-slots > 0");
        CHECK(reg_stat(cs, "capture-contract-version") == 1, "capture-contract-version == 1");
        CHECK(reg_stat(cs, "extension-kit-version") == 2, "extension-kit-version == 2");
    }

    const auto fastpath_before = reg_stat(cs, "fastpath-hits");

    // AC2: map/filter hot path uses slot_lookup_fast
    {
        std::println("\n--- AC2: map/filter fastpath hits ---");
        (void)cs.eval("(define big (list 1 2 3 4 5 6 7 8 9 10))");
        (void)cs.eval("(map not big)");
        (void)cs.eval("(filter null? big)");
        const auto fastpath_after = reg_stat(cs, "fastpath-hits");
        CHECK(fastpath_after > fastpath_before,
              std::format("fastpath-hits grew ({} -> {})", fastpath_before, fastpath_after));
    }

    // AC3: regex invalid pattern uses make_primitive_error (not silent catch)
    {
        std::println("\n--- AC3: regex error consistency ---");
        const auto err_before = cs.evaluator().get_primitive_error_count();
        auto r = cs.eval("(regex-match? \"[\" \"test\")");
        const auto err_after = cs.evaluator().get_primitive_error_count();
        CHECK(err_after > err_before,
              std::format("invalid regex bumps error counter ({} -> {})", err_before, err_after));
        auto is_err = cs.eval("(error? (regex-match? \"[\" \"test\"))");
        CHECK(is_err && aura::compiler::types::is_bool(*is_err) &&
                  aura::compiler::types::as_bool(*is_err),
              "invalid regex-match? returns error value");
        (void)r;
    }

    // AC4: EDA extension kit + registry consistency
    {
        std::println("\n--- AC4: EDA registry consistency ---");
        CHECK(ext_stat(cs, "extension-kit-version") == 2, "extension-stats kit version == 2");
        CHECK(reg_stat(cs, "eda-registered") >= ext_stat(cs, "category-eda"),
              "registry eda-registered covers category-eda");
        CHECK(reg_stat(cs, "consistency-rate") > 0, "consistency-rate > 0");
        auto sk = cs.eval("(primitive:generate-skeleton \"eda interface modport mutate\")");
        CHECK(sk && aura::compiler::types::is_hash(*sk), "generate-skeleton still works post-#709");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) == 83,
              "stats:count == 83");
    }

    // AC6: fiber stress — map/filter + registry stats under concurrent eval
    {
        std::println("\n--- AC6: fiber stress ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 20;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(map not big)");
                (void)cs.eval("(filter null? big)");
                (void)cs.eval("(mutate:request-gc-safepoint)");
                auto r1 = cs.eval("(query:primitives-registry-stats)");
                auto r2 = cs.eval("(query:primitives-extension-stats)");
                if (r1 && aura::compiler::types::is_hash(*r1) && r2 &&
                    aura::compiler::types::is_hash(*r2)) {
                    ok_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() > 0,
              std::format("fiber stress produced {} successful registry queries", ok_count.load()));
        CHECK(reg_stat(cs, "fastpath-hits") > fastpath_before,
              "fastpath-hits accumulated under fiber stress");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}