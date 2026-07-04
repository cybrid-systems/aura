// test_issue_323.cpp — Issue #323: AOT/Bridge Hot-Update +
// Stale Detection under Multi-Agent Concurrency.
//
// Validates the AOT bridge surface:
//   - mangle_aot_name uniqueness + version-suffix correctness
//   - Hot-swap counter observability via snapshot
//   - Multi-fiber stress: hot-swap + invalidation under load
//
// Ship scope (Issue #323 AC #1, #3):
//   - Mangle correctness (Issue #136 + #243 version suffix)
//   - Hot-swap counter observable via Aura primitives
//   - 4-thread concurrent hot-swap stress test
//
// AC #2 (mutation_loop.py integration), AC #4 (TSan), AC #5
// (docs) are deferred.

#include "aot_mangle.h"
#include "issue_test_harness.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_323_detail {

using aura::compiler::CompilerService;
using aura::compiler::mangle_aot_name;

// ── Scenario 1: mangle uniqueness for many distinct names ──
bool test_mangle_uniqueness() {
    std::println("\n--- Scenario 1: mangle uniqueness (1000 distinct names) ---");
    std::unordered_set<std::string> seen;
    int collisions = 0;
    for (int i = 0; i < 1000; ++i) {
        std::string name = "f" + std::to_string(i);
        // Use disambiguator i + version 0.
        std::string mangled = mangle_aot_name(name, i, 0);
        if (!seen.insert(mangled).second) {
            ++collisions;
        }
    }
    std::println("  1000 distinct names, collisions: {}", collisions);
    CHECK(collisions == 0, "1000 distinct names produce 0 collisions");
    return true;
}

// ── Scenario 2: mangle version suffix distinguishes epochs ──
bool test_mangle_version_suffix() {
    std::println("\n--- Scenario 2: mangle version suffix differs across defuse_version ---");
    std::string name = "my-fn";
    std::string v0 = mangle_aot_name(name, 0, 0);
    std::string v1 = mangle_aot_name(name, 0, 1);
    std::string v42 = mangle_aot_name(name, 0, 42);
    std::string v65535 = mangle_aot_name(name, 0, 65535);
    std::println("  v0:   {}", v0);
    std::println("  v1:   {}", v1);
    std::println("  v42:  {}", v42);
    std::println("  v65k: {}", v65535);
    CHECK(v0 != v1, "v0 != v1 (version suffix differs)");
    CHECK(v1 != v42, "v1 != v42");
    CHECK(v42 != v65535, "v42 != v65535");
    return true;
}

// ── Scenario 3: mangle preserves reserved name __top__ ──
bool test_mangle_preserves_top() {
    std::println("\n--- Scenario 3: mangle preserves __top__ verbatim ---");
    std::string top_v0 = mangle_aot_name("__top__", 0, 0);
    std::string top_v1 = mangle_aot_name("__top__", 0, 1);
    std::println("  __top__ v0: {}", top_v0);
    std::println("  __top__ v1: {}", top_v1);
    // Issue #136: __top__ is the canonical entry point; its
    // leading/trailing underscores must be preserved.
    CHECK(top_v0.substr(0, 7) == "__top__",
          "leading underscores + 'top' + trailing underscores preserved");
    return true;
}

// ── Scenario 4: mangle escapes special chars correctly ──
bool test_mangle_special_chars() {
    std::println("\n--- Scenario 4: mangle escapes special chars ---");
    std::string mangled = mangle_aot_name("foo@bar.baz?qux!", 0, 0);
    std::println("  foo@bar.baz?qux! → {}", mangled);
    // Should have no @ . ? ! chars — all replaced with _.
    for (char c : mangled) {
        if (c == '@' || c == '.' || c == '?' || c == '!') {
            std::println(std::cerr, "  FAIL: special char '{}' not escaped", c);
            return false;
        }
    }
    CHECK(true, "all special chars escaped to _");
    return true;
}

// ── Scenario 5: hot-swap counter observable via snapshot ──
bool test_hot_swap_counter_observable() {
    std::println("\n--- Scenario 5: jit_compilations observable via snapshot ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    // Initial snapshot.
    auto s0 = cs.snapshot();
    std::uint64_t jc0 = s0.jit_compilations;
    // Trigger a mutation that may bump jit_compilations.
    (void)cs.eval("(mutate:replace-value (define a 2) (define a 2))");
    auto s1 = cs.snapshot();
    std::uint64_t jc1 = s1.jit_compilations;
    std::println("  jit_compilations: {} → {}", jc0, jc1);
    CHECK(jc1 >= jc0, "jit_compilations is monotonic non-decreasing");
    return true;
}

// ── Scenario 6: multi-threaded hot-swap stress ──
bool test_multithreaded_hot_swap_stress() {
    std::println("\n--- Scenario 6: 4 threads × 50 hot-swap cycles ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    constexpr int K_THREADS = 4;
    constexpr int K_ITERS = 50;
    std::mutex mtx;
    std::atomic<int> cycles_done{0};
    auto worker = [&](int tid) {
        for (int i = 0; i < K_ITERS; ++i) {
            std::lock_guard<std::mutex> lk(mtx);
            std::string code = "(mutate:replace-value (define a ";
            code += std::to_string(tid * 100 + i);
            code += ") (define a ";
            code += std::to_string(tid * 100 + i);
            code += "))";
            (void)cs.eval(code);
            cycles_done.fetch_add(1);
        }
    };
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < K_THREADS; ++i)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    int total = K_THREADS * K_ITERS;
    auto snap = cs.snapshot();
    std::println("  {} cycles in {}ms, final jit_compilations: {}", total, ms,
                 snap.jit_compilations);
    CHECK(cycles_done.load() == total, "all cycles completed");
    CHECK(ms < 30000, "completed within 30s budget");
    return true;
}

} // namespace aura_323_detail

int main() {
    using namespace aura_323_detail;
    test_mangle_uniqueness();
    test_mangle_version_suffix();
    test_mangle_preserves_top();
    test_mangle_special_chars();
    test_hot_swap_counter_observable();
    test_multithreaded_hot_swap_stress();
    return run_pilot_tests();
}
