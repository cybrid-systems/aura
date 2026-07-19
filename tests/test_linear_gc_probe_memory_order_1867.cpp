// @category: unit
// @reason: Issue #1867 — record_linear_gc_probe violation counters use
// memory_order_release; audit/stats readers acquire-load so violations
// are not missed under concurrent probe vs dashboard.
//
//   AC1: source uses release on violation path; acquire on key loads
//   AC2: sequential violation probe increments counter (visible)
//   AC3: concurrent probes + acquire-load reader see final count

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: release writes / acquire loads ---");
        auto gc = read_first({"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"});
        auto q = read_first({"src/compiler/evaluator_primitives_query.cpp",
                             "../src/compiler/evaluator_primitives_query.cpp"});
        CHECK(!gc.empty(), "read evaluator_gc.cpp");
        CHECK(gc.find("#1867") != std::string::npos, "cites #1867");
        auto pos = gc.find("record_linear_gc_probe");
        CHECK(pos != std::string::npos, "record_linear_gc_probe present");
        auto win = gc.substr(pos, 1800);
        CHECK(win.find("memory_order_release") != std::string::npos, "violation path uses release");
        CHECK(win.find("linear_violations_caught_total") != std::string::npos,
              "bumps violations_caught");
        // Success path still relaxed (non-safety tallies).
        CHECK(win.find("linear_check_pass_count_") != std::string::npos ||
                  win.find("memory_order_relaxed") != std::string::npos,
              "pass path / relaxed still present");
        CHECK(!q.empty(), "read query.cpp");
        CHECK(q.find("linear_violations_caught_total.load(std::memory_order_acquire)") !=
                  std::string::npos,
              "stats load uses acquire");
        CHECK(q.find("#1867") != std::string::npos, "query cites #1867");
    }

    // ── AC2: sequential visibility ──
    {
        std::println("\n--- AC2: sequential violation visible ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m != nullptr, "service wires metrics");
        const auto v0 = m->linear_violations_caught_total.load(std::memory_order_acquire);
        // Stale frame version → violation via check_linear_ownership_for_frame
        // (same path ends in record_linear_gc_probe on related probes).
        ev.bump_defuse_version_for_test();
        const bool bad = ev.check_linear_ownership_for_frame(0, /*linear_state=*/1);
        CHECK(!bad, "stale frame fails check");
        const auto v1 = m->linear_violations_caught_total.load(std::memory_order_acquire);
        CHECK(v1 == v0 + 1, "violation counter +1 after probe");
        // GC safepoint probe on empty/moved setup should not crash.
        ev.test_probe_linear_at_gc_safepoint();
        CHECK(true, "safepoint probe ok");
    }

    // ── AC3: concurrent probe + acquire reader ──
    {
        std::println("\n--- AC3: concurrent probes + acquire reader ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m != nullptr, "metrics wired");
        ev.bump_defuse_version_for_test();

        std::atomic<bool> start{false};
        std::atomic<std::uint64_t> ops{0};
        constexpr int kWriters = 4;
        constexpr int kIters = 100;
        std::vector<std::thread> threads;
        threads.reserve(kWriters + 1);

        for (int w = 0; w < kWriters; ++w) {
            threads.emplace_back([&] {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (int i = 0; i < kIters; ++i) {
                    (void)ev.check_linear_ownership_for_frame(0, /*linear_state=*/1);
                    ops.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        std::atomic<std::uint64_t> max_seen{0};
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < kIters * 4; ++i) {
                auto v = m->linear_violations_caught_total.load(std::memory_order_acquire);
                auto prev = max_seen.load(std::memory_order_relaxed);
                while (v > prev && !max_seen.compare_exchange_weak(prev, v)) {
                }
            }
        });

        const auto before = m->linear_violations_caught_total.load(std::memory_order_acquire);
        start.store(true, std::memory_order_release);
        for (auto& t : threads)
            t.join();
        const auto after = m->linear_violations_caught_total.load(std::memory_order_acquire);
        CHECK(ops.load() == static_cast<std::uint64_t>(kWriters * kIters), "all writer ops done");
        CHECK(after >= before + static_cast<std::uint64_t>(kWriters * kIters),
              "all violations recorded");
        CHECK(max_seen.load() >= before + 1, "reader observed progress under acquire");
    }

    std::println("\n=== test_linear_gc_probe_memory_order_1867: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
