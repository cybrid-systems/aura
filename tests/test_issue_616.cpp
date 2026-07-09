// @category: integration
// @reason: Issue #616 EDA hardware-co-design primitives (load-sv,
// parse-verification-result, query:eda-hw-stats)
//
// Scope-limited close matching the #601 / #491 / #479 / #604 / #606 /
// #614 / #615 pattern: ship the file-boundary EDA primitives
// (load-sv / parse-verification-result) + dedicated observability
// primitive (query:eda-hw-stats) + 4 new counters now; the larger
// Phase 2/3 work from #499 (full simulator FFI integration,
// property/cover manipulation, hardware intrinsics surface) remains
// a separate follow-up.

#include <atomic>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_616_detail {
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

// Read an integer field from a hash returned by Aura.
static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view hash_src,
                             std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_src, key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

// Read a string field from a hash returned by Aura (echoed as
// (hash-ref h 'k') → string). Returns empty string on miss.
static std::string hash_string(aura::compiler::CompilerService& cs, std::string_view hash_src,
                               std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_src, key));
    if (!r || !aura::compiler::types::is_string(*r))
        return {};
    const auto& heap = cs.evaluator().string_heap();
    return std::string(heap[aura::compiler::types::as_string_idx(*r)]);
}

// Read a query:eda-hw-stats field directly.
static std::int64_t hw_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    return hash_int(cs, "(query:eda-hw-stats)", key);
}

} // namespace aura_issue_616_detail

int aura_issue_616_run() {
    using namespace aura_issue_616_detail;
    std::println("=== Issue #616: EDA hardware-co-design primitives (load-sv + "
                 "parse-verification-result + query:eda-hw-stats) ===");

    aura::compiler::CompilerService cs;

    // AC1: (eda:load-sv path) on a valid SV file returns a hash with
    // node-count > 0, status-ok == 1, and the echoed path string.
    // Note: each hash_int/hash_string call below re-evaluates the
    // primitive, so the counter bumps by N+1 (1 for the initial
    // cs.eval that captures h, plus N for each field lookup).
    {
        std::println("\n--- AC1: (eda:load-sv) on valid sample.sv ---");
        const std::string fixture = "tests/fixtures/issue_616/sample.sv";
        const auto load_ok_before = hw_stat(cs, "load-sv-total");
        const auto load_fail_before = hw_stat(cs, "load-sv-failure-total");
        auto h = cs.eval(std::format("(eda:load-sv \"{}\")", fixture));
        CHECK(h && aura::compiler::types::is_hash(*h), "(eda:load-sv) returns a hash");
        auto eval_str = std::format("(eda:load-sv \"{}\")", fixture);
        const auto node_count = hash_int(cs, eval_str, "node-count");
        const auto status_ok = hash_int(cs, eval_str, "status-ok");
        const auto path_echo = hash_string(cs, eval_str, "path");
        CHECK(node_count > 0, std::format("node-count > 0 (got {})", node_count));
        CHECK(status_ok == 1, std::format("status-ok == 1 (got {})", status_ok));
        CHECK(path_echo == fixture, std::format("path echoed correctly (got '{}')", path_echo));
        const auto load_ok_after = hw_stat(cs, "load-sv-total");
        const auto load_fail_after = hw_stat(cs, "load-sv-failure-total");
        // 1 (initial h) + 3 (hash_int x2 + hash_string x1) = 4 calls.
        CHECK(load_ok_after == load_ok_before + 4,
              std::format("load-sv-total bumped +4 (1 initial + 3 field lookups) ({} -> {})",
                          load_ok_before, load_ok_after));
        CHECK(load_fail_after == load_fail_before,
              std::format("load-sv-failure-total unchanged ({} -> {})", load_fail_before,
                          load_fail_after));
    }

    // AC2: (eda:load-sv path) on a missing file returns the hash
    // with node-count == 0, status-ok == 0, reason set; load-sv-
    // failure-total bumps. 1 (initial h) + 4 (2 hash_int + 2 hash_string)
    // = 5 calls.
    {
        std::println("\n--- AC2: (eda:load-sv) on missing path ---");
        const std::string missing = "/tmp/this-file-does-not-exist-issue616.sv";
        const auto load_fail_before = hw_stat(cs, "load-sv-failure-total");
        const auto eval_str = std::format("(eda:load-sv \"{}\")", missing);
        auto h = cs.eval(eval_str);
        CHECK(h && aura::compiler::types::is_hash(*h),
              "(eda:load-sv) on missing path returns a hash");
        const auto node_count = hash_int(cs, eval_str, "node-count");
        const auto status_ok = hash_int(cs, eval_str, "status-ok");
        const auto reason = hash_string(cs, eval_str, "reason");
        const auto path_echo = hash_string(cs, eval_str, "path");
        CHECK(node_count == 0, std::format("node-count == 0 on missing path (got {})", node_count));
        CHECK(status_ok == 0, std::format("status-ok == 0 on missing path (got {})", status_ok));
        CHECK(!reason.empty(), std::format("reason is non-empty (got '{}')", reason));
        CHECK(path_echo == missing,
              std::format("path echoed even on missing (got '{}')", path_echo));
        const auto load_fail_after = hw_stat(cs, "load-sv-failure-total");
        CHECK(load_fail_after == load_fail_before + 5,
              std::format("load-sv-failure-total bumped +5 (1+4 field lookups) ({} -> {})",
                          load_fail_before, load_fail_after));
    }

    // AC3: (eda:parse-verification-result path) on a valid JSON
    // returns a hash with coverage-pct, assertion-pass,
    // assertion-fail populated and success == 1.
    // 1 (initial h) + 5 (4 hash_int + 1 hash_string) = 6 calls.
    {
        std::println("\n--- AC3: (eda:parse-verification-result) on valid cov.json ---");
        const std::string fixture = "tests/fixtures/issue_616/cov.json";
        const auto parse_ok_before = hw_stat(cs, "parse-verification-result-total");
        const auto eval_str = std::format("(eda:parse-verification-result \"{}\")", fixture);
        auto h = cs.eval(eval_str);
        CHECK(h && aura::compiler::types::is_hash(*h),
              "(eda:parse-verification-result) returns a hash");
        const auto cov = hash_int(cs, eval_str, "coverage-pct");
        const auto pass = hash_int(cs, eval_str, "assertion-pass");
        const auto fail = hash_int(cs, eval_str, "assertion-fail");
        const auto success = hash_int(cs, eval_str, "success");
        const auto path_echo = hash_string(cs, eval_str, "path");
        CHECK(cov == 87, std::format("coverage-pct == 87 (got {})", cov));
        CHECK(pass == 42, std::format("assertion-pass == 42 (got {})", pass));
        CHECK(fail == 3, std::format("assertion-fail == 3 (got {})", fail));
        CHECK(success == 1, std::format("success == 1 (got {})", success));
        CHECK(path_echo == fixture, std::format("path echoed correctly (got '{}')", path_echo));
        const auto parse_ok_after = hw_stat(cs, "parse-verification-result-total");
        CHECK(parse_ok_after == parse_ok_before + 6,
              std::format("parse-verification-result-total bumped +6 (1+5 lookups) ({} -> {})",
                          parse_ok_before, parse_ok_after));
    }

    // AC4: (eda:parse-verification-result) on a missing file
    // returns success == 0 and bumps parse-verification-failure-total.
    // 1 (initial h) + 1 (hash_int) = 2 calls.
    {
        std::println("\n--- AC4: (eda:parse-verification-result) on missing path ---");
        const std::string missing = "/tmp/cov-does-not-exist-issue616.json";
        const auto parse_fail_before = hw_stat(cs, "parse-verification-failure-total");
        const auto eval_str = std::format("(eda:parse-verification-result \"{}\")", missing);
        auto h = cs.eval(eval_str);
        CHECK(h && aura::compiler::types::is_hash(*h),
              "(eda:parse-verification-result) on missing path returns a hash");
        const auto success = hash_int(cs, eval_str, "success");
        CHECK(success == 0, std::format("success == 0 on missing path (got {})", success));
        const auto parse_fail_after = hw_stat(cs, "parse-verification-failure-total");
        CHECK(parse_fail_after == parse_fail_before + 2,
              std::format("parse-verification-failure-total bumped +2 (1+1 lookup) ({} -> {})",
                          parse_fail_before, parse_fail_after));
    }

    // AC5: (query:eda-hw-stats) shape — 6 fields, all non-negative,
    // success rates are within [0, 100].
    {
        std::println("\n--- AC5: (query:eda-hw-stats) shape ---");
        auto h = cs.eval("(query:eda-hw-stats)");
        CHECK(h && aura::compiler::types::is_hash(*h), "(query:eda-hw-stats) returns a hash");
        const auto load_ok = hw_stat(cs, "load-sv-total");
        const auto load_fail = hw_stat(cs, "load-sv-failure-total");
        const auto parse_ok = hw_stat(cs, "parse-verification-result-total");
        const auto parse_fail = hw_stat(cs, "parse-verification-failure-total");
        const auto load_rate = hw_stat(cs, "load-sv-success-rate");
        const auto parse_rate = hw_stat(cs, "parse-verification-success-rate");
        CHECK(load_ok >= 1, std::format("load-sv-total >= 1 (got {})", load_ok));
        CHECK(load_fail >= 1, std::format("load-sv-failure-total >= 1 (got {})", load_fail));
        CHECK(parse_ok >= 1,
              std::format("parse-verification-result-total >= 1 (got {})", parse_ok));
        CHECK(parse_fail >= 1,
              std::format("parse-verification-failure-total >= 1 (got {})", parse_fail));
        CHECK(load_rate >= 0 && load_rate <= 100,
              std::format("load-sv-success-rate in [0,100] (got {})", load_rate));
        CHECK(parse_rate >= 0 && parse_rate <= 100,
              std::format("parse-verification-success-rate in [0,100] (got {})", parse_rate));
    }

    // AC6: concurrent eda:load-sv + eda:parse-verification-result
    // under 2 threads × 4 iters each. Atomic counter wiring check.
    {
        std::println("\n--- AC6: concurrent load-sv + parse-verification-result ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 4;
        const auto load_ok_before = hw_stat(cs, "load-sv-total");
        const auto parse_ok_before = hw_stat(cs, "parse-verification-result-total");
        auto worker_load = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(eda:load-sv \"tests/fixtures/issue_616/sample.sv\")");
                if (r && aura::compiler::types::is_hash(*r))
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        auto worker_parse = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval(
                    "(eda:parse-verification-result \"tests/fixtures/issue_616/cov.json\")");
                if (r && aura::compiler::types::is_hash(*r))
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker_load);
        std::thread t2(worker_parse);
        t1.join();
        t2.join();
        const auto load_ok_after = hw_stat(cs, "load-sv-total");
        const auto parse_ok_after = hw_stat(cs, "parse-verification-result-total");
        CHECK(ok_count.load() == k_iters * 2,
              std::format("concurrent: {} / {} calls returned hash", ok_count.load(), k_iters * 2));
        CHECK(load_ok_after == load_ok_before + k_iters,
              std::format("load-sv-total bumped +{} under concurrency ({} -> {})", k_iters,
                          load_ok_before, load_ok_after));
        CHECK(parse_ok_after == parse_ok_before + k_iters,
              std::format("parse-verification-result-total bumped +{} under concurrency ({} -> {})",
                          k_iters, parse_ok_before, parse_ok_after));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_616_run();
}
#endif
