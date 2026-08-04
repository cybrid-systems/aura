// @category: unit
// @reason: Issue #2523 — residual optimistic/region workspace concurrency
// after #2121: contention stats + soft path for disjoint multi-Agent mutate.
//
//   AC1: Source cites #2523; residual strategy documented
//   AC2: Two threads on disjoint regions do not both take global exclusive
//        (measured via new contention stats / optimistic hits)
//   AC3: Cross-region / policy-off / try_acquire still GlobalExclusive
//   AC4: Mixed multi-thread stress completes (PCV/StableNodeRef lineage)
//   AC5: query:workspace-mtx-contention-stats surfaces hold/wait/collision
//   AC6: N≥4 region agents measurable win vs pure exclusive baseline
//   AC7: this registered issue test

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view query, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", query, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void hold_work(int spins) {
    volatile std::uint64_t x = 1;
    for (int i = 0; i < spins; ++i)
        x = x * 1664525u + 1013904223u;
    (void)x;
}

// ── AC1 ──
static void ac1_source_docs() {
    std::println("\n--- AC1: source cites #2523 + residual strategy ---");
    auto src = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(!src.empty(), "read mutation boundary");
    CHECK(src.find("#2523") != std::string::npos, "cites #2523");
    CHECK(src.find("workspace_mtx_optimistic_hit_total") != std::string::npos ||
              src.find("optimistic_hit") != std::string::npos,
          "optimistic hit metric");
    CHECK(src.find("Soft path residual") != std::string::npos ||
              src.find("query:workspace-mtx-contention-stats") != std::string::npos,
          "residual strategy documented");
    CHECK(src.find("GlobalExclusive") != std::string::npos, "global exclusive documented");
    CHECK(src.find("RegionExclusive") != std::string::npos ||
              src.find("try_acquire_for_region") != std::string::npos,
          "region soft path documented");
    auto fiber = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(fiber.find("#2523") != std::string::npos, "orch soft path cites #2523");
    CHECK(fiber.find("try_acquire_for_region") != std::string::npos,
          "orch host prefers region acquire");
    auto met = read_file("src/compiler/observability_metrics.h");
    CHECK(met.find("workspace_mtx_optimistic_hit_total") != std::string::npos,
          "metrics optimistic hit");
    CHECK(met.find("workspace_mtx_region_collision_total") != std::string::npos,
          "metrics region collision");
    CHECK(met.find("#2523") != std::string::npos, "metrics cite #2523");
}

// ── AC2 ──
static void ac2_disjoint_not_dual_global() {
    std::println("\n--- AC2: disjoint regions avoid dual global exclusive ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_workspace_region_concurrency_enabled(true);
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "metrics");

    const auto g0 = m->workspace_global_exclusive_total.load(std::memory_order_relaxed);
    const auto o0 = m->workspace_mtx_optimistic_hit_total.load(std::memory_order_relaxed);
    const auto r0 = m->workspace_region_acquire_total.load(std::memory_order_relaxed);

    constexpr int kN = 4;
    std::atomic<int> ready{0};
    std::atomic<int> go{0};
    std::atomic<int> region_mode{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kN; ++i) {
        threads.emplace_back([&, i]() {
            ready.fetch_add(1);
            while (go.load() == 0)
                std::this_thread::yield();
            bool ok = true;
            const auto key =
                Evaluator::workspace_region_key_from_name(std::format("agent-define-{}", i));
            auto gr = Evaluator::MutationBoundaryGuard::try_acquire_for_region(ev, key, 1, &ok);
            if (gr && *gr) {
                if ((*gr)->is_region_mode())
                    region_mode.fetch_add(1);
                hold_work(150'000);
            }
        });
    }
    while (ready.load() < kN)
        std::this_thread::yield();
    go.store(1);
    for (auto& t : threads)
        t.join();

    CHECK(m->workspace_region_acquire_total.load(std::memory_order_relaxed) >=
              r0 + static_cast<std::uint64_t>(kN),
          "region acquires += N");
    CHECK(m->workspace_mtx_optimistic_hit_total.load(std::memory_order_relaxed) >=
              o0 + static_cast<std::uint64_t>(kN),
          "optimistic hits += N");
    CHECK(m->workspace_global_exclusive_total.load(std::memory_order_relaxed) == g0,
          "no global exclusive for pure region holds");
    CHECK(region_mode.load() == kN, "all N region_mode");

    // AC5 surface reflects optimistic hits
    CHECK(href(cs, "query:workspace-mtx-contention-stats", "workspace-mtx-optimistic-hit-total") >=
              static_cast<std::int64_t>(kN),
          "contention-stats optimistic hits");
}

// ── AC3 ──
static void ac3_global_and_fallback() {
    std::println("\n--- AC3: global exclusive for topology / policy-off ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());

    {
        const auto g0 = m->workspace_global_exclusive_total.load(std::memory_order_relaxed);
        bool ok = true;
        auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, 1, &ok);
        CHECK(gr.has_value() && *gr, "global acquire");
        CHECK(!(*gr)->is_region_mode(), "try_acquire is GlobalExclusive");
        CHECK(m->workspace_global_exclusive_total.load(std::memory_order_relaxed) == g0 + 1,
              "global exclusive +1");
    }

    {
        ev.set_workspace_region_concurrency_enabled(false);
        const auto fb0 = m->workspace_region_fallback_global_total.load(std::memory_order_relaxed);
        const auto g0 = m->workspace_global_exclusive_total.load(std::memory_order_relaxed);
        bool ok = true;
        auto gr = Evaluator::MutationBoundaryGuard::try_acquire_for_region(ev, 99, 1, &ok);
        CHECK(gr.has_value() && *gr, "fallback acquire");
        CHECK(!(*gr)->is_region_mode(), "policy off → not region");
        CHECK(m->workspace_region_fallback_global_total.load(std::memory_order_relaxed) >= fb0 + 1,
              "fallback counter");
        CHECK(m->workspace_global_exclusive_total.load(std::memory_order_relaxed) == g0 + 1,
              "fallback uses global");
        ev.set_workspace_region_concurrency_enabled(true);
    }
}

// ── AC4 ──
static void ac4_mixed_stress() {
    std::println("\n--- AC4: mixed region + global stress (TSan lineage) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_workspace_region_concurrency_enabled(true);
    std::atomic<int> done{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i) {
        threads.emplace_back([&, i]() {
            for (int j = 0; j < 24; ++j) {
                bool ok = true;
                if ((i + j) % 3 == 0) {
                    auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, 1, &ok);
                    if (gr && *gr)
                        hold_work(4'000);
                } else {
                    auto gr = Evaluator::MutationBoundaryGuard::try_acquire_for_region(
                        ev, static_cast<std::uint64_t>(i * 19 + j), 1, &ok);
                    if (gr && *gr)
                        hold_work(4'000);
                }
            }
            done.fetch_add(1);
        });
    }
    for (auto& t : threads)
        t.join();
    CHECK(done.load() == 6, "all stress threads finished");
}

// ── AC5 ──
static void ac5_contention_query() {
    std::println("\n--- AC5: query:workspace-mtx-contention-stats ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_workspace_region_concurrency_enabled(true);
    {
        bool ok = true;
        const auto key = Evaluator::workspace_region_key_from_name("ac5-define");
        auto gr = Evaluator::MutationBoundaryGuard::try_acquire_for_region(ev, key, 1, &ok);
        CHECK(gr.has_value() && *gr, "region hold for query");
        hold_work(10'000);
    }
    auto h = cs.eval("(engine:metrics \"query:workspace-mtx-contention-stats\")");
    CHECK(h && is_hash(*h), "contention-stats hash");
    CHECK(href(cs, "query:workspace-mtx-contention-stats", "schema-2523") == 2523, "schema-2523");
    CHECK(href(cs, "query:workspace-mtx-contention-stats", "workspace-mtx-contention-wired") == 1,
          "wired");
    CHECK(href(cs, "query:workspace-mtx-contention-stats", "workspace-mtx-acquire-total") >= 1,
          "acquire");
    CHECK(href(cs, "query:workspace-mtx-contention-stats", "workspace-mtx-wait-ns-total") >= 0,
          "wait ns");
    CHECK(href(cs, "query:workspace-mtx-contention-stats", "workspace-mtx-waiters-peak") >= 0,
          "waiters peak");
    CHECK(href(cs, "query:workspace-mtx-contention-stats", "workspace-mtx-hold-ns-p99") >= 0,
          "hold ns p99");
    CHECK(href(cs, "query:workspace-mtx-contention-stats", "workspace-mtx-optimistic-hit-total") >=
              1,
          "optimistic hit");
    CHECK(href(cs, "query:workspace-mtx-contention-stats",
               "workspace-mtx-region-collision-total") >= 0,
          "region collision");
    CHECK(href(cs, "query:workspace-mtx-contention-stats", "workspace-region-collision-rate-pct") >=
              0,
          "collision rate");
    // Lineage keys on hold-stats too
    CHECK(href(cs, "query:mutation-boundary-hold-stats", "schema-2523") == 2523,
          "hold-stats schema-2523");
    CHECK(href(cs, "query:mutation-boundary-hold-stats", "workspace-mtx-optimistic-hit-total") >= 1,
          "hold-stats optimistic hit");
}

// ── AC6 ──
static void ac6_throughput() {
    std::println("\n--- AC6: N=4 region ≥1.3× vs global baseline ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_workspace_region_concurrency_enabled(true);

    constexpr int kN = 4;
    constexpr int kSpins = 350'000;

    auto run_global = [&]() {
        std::atomic<int> ready{0};
        std::atomic<int> go{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < kN; ++i) {
            threads.emplace_back([&]() {
                ready.fetch_add(1);
                while (go.load() == 0)
                    std::this_thread::yield();
                bool ok = true;
                auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, 1, &ok);
                if (gr && *gr)
                    hold_work(kSpins);
            });
        }
        while (ready.load() < kN)
            std::this_thread::yield();
        const auto t0 = std::chrono::steady_clock::now();
        go.store(1);
        for (auto& t : threads)
            t.join();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    auto run_region = [&]() {
        std::atomic<int> ready{0};
        std::atomic<int> go{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < kN; ++i) {
            threads.emplace_back([&, i]() {
                ready.fetch_add(1);
                while (go.load() == 0)
                    std::this_thread::yield();
                bool ok = true;
                const auto key =
                    Evaluator::workspace_region_key_from_name(std::format("bench-define-{}", i));
                auto gr = Evaluator::MutationBoundaryGuard::try_acquire_for_region(ev, key, 1, &ok);
                if (gr && *gr)
                    hold_work(kSpins);
            });
        }
        while (ready.load() < kN)
            std::this_thread::yield();
        const auto t0 = std::chrono::steady_clock::now();
        go.store(1);
        for (auto& t : threads)
            t.join();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    (void)run_global();
    (void)run_region();
    const double t_global = run_global();
    const double t_region = run_region();
    const double speedup = t_global / (t_region > 1e-9 ? t_region : 1e-9);
    std::println("  global={:.4f}s  region={:.4f}s  speedup={:.2f}×", t_global, t_region, speedup);
    // Soft CI floor 1.3× (aspirational 1.5×); must still beat exclusive.
    CHECK(speedup >= 1.3, "region path ≥1.3× throughput vs global unique (N=4)");
}

} // namespace

int run_test_workspace_mtx_contention_2523() {
    std::println("=== Issue #2523: workspace_mtx residual contention + soft path ===");
    ac1_source_docs();
    ac2_disjoint_not_dual_global();
    ac3_global_and_fallback();
    ac4_mixed_stress();
    ac5_contention_query();
    ac6_throughput();
    std::println("\n=== #2523: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_workspace_mtx_contention_2523();
}
#endif
