// @category: unit
// @reason: Issue #2121 — region-based workspace write concurrency for
// multi-Agent disjoint-Define mutates.
//
// Strategy (see evaluator_mutation_boundary.cpp #2121 header):
//   GlobalExclusive: unique_lock(workspace_mtx_) — default / topology / batch
//   RegionExclusive: shared_lock(workspace) + unique region shard
//
//   AC1: source cites #2121 + documents region strategy
//   AC2: two threads on disjoint regions do not both take global unique
//   AC3: global try_acquire still takes GlobalExclusive; atomic-batch falls back
//   AC4: concurrent region + global stress completes without crash
//   AC5: query:mutation-boundary-hold-stats schema-2121 (region keys)
//   AC6: N=4 region agents ≥1.5× wall-time vs global unique baseline
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    // AC5: region counters live on existing hold-stats (SlimSurface freeze —
    // no new query:*-stats name). Equivalent to query:workspace-mtx-contention-stats.
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:mutation-boundary-hold-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Busy work that keeps the Guard body alive without FlatAST topology races.
static void hold_work(int spins) {
    volatile std::uint64_t x = 1;
    for (int i = 0; i < spins; ++i)
        x = x * 1664525u + 1013904223u;
    (void)x;
}

static void ac1_source_docs() {
    std::println("\n--- AC1: source cites #2121 + strategy ---");
    auto src = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(!src.empty(), "read mutation boundary source");
    CHECK(src.find("#2121") != std::string::npos, "cites #2121");
    CHECK(src.find("RegionExclusive") != std::string::npos ||
              src.find("region-based") != std::string::npos ||
              src.find("try_acquire_for_region") != std::string::npos,
          "region strategy documented");
    CHECK(src.find("GlobalExclusive") != std::string::npos ||
              src.find("global unique") != std::string::npos,
          "global exclusive fallback documented");
    auto ixx = read_file("src/compiler/evaluator.ixx");
    CHECK(ixx.find("try_acquire_for_region") != std::string::npos, "API declared");
    CHECK(ixx.find("kWorkspaceRegionShards") != std::string::npos, "shard count declared");
}

static void ac2_disjoint_regions_not_global_unique() {
    std::println("\n--- AC2: disjoint regions avoid dual global unique ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_workspace_region_concurrency_enabled(true);
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "metrics");

    const auto g0 = m->workspace_global_exclusive_total.load(std::memory_order_relaxed);
    const auto r0 = m->workspace_region_acquire_total.load(std::memory_order_relaxed);

    constexpr int kN = 4;
    std::atomic<int> ready{0};
    std::atomic<int> go{0};
    std::atomic<int> region_mode_count{0};
    std::vector<std::thread> threads;
    threads.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        threads.emplace_back([&, i]() {
            ready.fetch_add(1);
            while (go.load() == 0)
                std::this_thread::yield();
            bool ok = true;
            // Distinct region keys that map to different shards when possible.
            const std::uint64_t key =
                Evaluator::workspace_region_key_from_name(std::format("define-{}", i));
            auto gr = Evaluator::MutationBoundaryGuard::try_acquire_for_region(ev, key, 1, &ok);
            CHECK(gr.has_value() && *gr, "region acquire");
            if (gr && *gr) {
                if ((*gr)->is_region_mode())
                    region_mode_count.fetch_add(1);
                hold_work(200'000);
            }
        });
    }
    while (ready.load() < kN)
        std::this_thread::yield();
    go.store(1);
    for (auto& t : threads)
        t.join();

    const auto g1 = m->workspace_global_exclusive_total.load(std::memory_order_relaxed);
    const auto r1 = m->workspace_region_acquire_total.load(std::memory_order_relaxed);
    CHECK(r1 >= r0 + static_cast<std::uint64_t>(kN), "region acquires += N");
    // Disjoint region path must not bump global exclusive for these N holds.
    CHECK(g1 == g0, "no global exclusive for pure region acquires");
    CHECK(region_mode_count.load() == kN, "all N guards report region_mode");
}

static void ac3_global_and_fallback() {
    std::println("\n--- AC3: global exclusive + policy fallback ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());

    // Global try_acquire always GlobalExclusive.
    {
        const auto g0 = m->workspace_global_exclusive_total.load(std::memory_order_relaxed);
        bool ok = true;
        auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, 1, &ok);
        CHECK(gr.has_value() && *gr, "global acquire");
        CHECK(!(*gr)->is_region_mode(), "try_acquire is not region mode");
        CHECK(m->workspace_global_exclusive_total.load(std::memory_order_relaxed) == g0 + 1,
              "global exclusive +1");
    }

    // Policy OFF → region request falls back to global.
    {
        ev.set_workspace_region_concurrency_enabled(false);
        const auto fb0 = m->workspace_region_fallback_global_total.load(std::memory_order_relaxed);
        const auto g0 = m->workspace_global_exclusive_total.load(std::memory_order_relaxed);
        bool ok = true;
        auto gr = Evaluator::MutationBoundaryGuard::try_acquire_for_region(ev, 42, 1, &ok);
        CHECK(gr.has_value() && *gr, "fallback acquire");
        CHECK(!(*gr)->is_region_mode(), "policy off → not region mode");
        CHECK(m->workspace_region_fallback_global_total.load(std::memory_order_relaxed) >= fb0 + 1,
              "fallback counter");
        CHECK(m->workspace_global_exclusive_total.load(std::memory_order_relaxed) == g0 + 1,
              "fallback uses global exclusive");
        ev.set_workspace_region_concurrency_enabled(true);
    }
}

static void ac4_mixed_stress() {
    std::println("\n--- AC4: mixed region + global stress ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_workspace_region_concurrency_enabled(true);
    std::atomic<int> done{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i) {
        threads.emplace_back([&, i]() {
            for (int j = 0; j < 20; ++j) {
                bool ok = true;
                if ((i + j) % 3 == 0) {
                    auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, 1, &ok);
                    if (gr && *gr)
                        hold_work(5'000);
                } else {
                    auto gr = Evaluator::MutationBoundaryGuard::try_acquire_for_region(
                        ev, static_cast<std::uint64_t>(i * 17 + j), 1, &ok);
                    if (gr && *gr)
                        hold_work(5'000);
                }
            }
            done.fetch_add(1);
        });
    }
    for (auto& t : threads)
        t.join();
    CHECK(done.load() == 6, "all stress threads finished");
}

static void ac5_query_schema() {
    std::println("\n--- AC5: hold-stats schema-2121 region contention keys ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
    CHECK(href(cs, "schema-2121") == 2121, "schema-2121");
    CHECK(href(cs, "issue-2121") == 2121, "issue-2121");
    CHECK(href(cs, "region-concurrency-wired") == 1, "wired");
    CHECK(href(cs, "workspace-region-shards") ==
              static_cast<std::int64_t>(Evaluator::kWorkspaceRegionShards),
          "shards");
    CHECK(href(cs, "workspace-mtx-acquire-total") >= 0, "acquire key");
    CHECK(href(cs, "workspace-region-acquire-total") >= 0, "region acquire key");
    CHECK(href(cs, "workspace-region-collision-total") >= 0, "collision key");
    CHECK(href(cs, "workspace-global-exclusive-total") >= 0, "global exclusive key");
    CHECK(href(cs, "workspace-region-concurrency-enabled") == 1, "policy on");
}

static void ac6_throughput_speedup() {
    std::println("\n--- AC6: N=4 region ≥1.5× vs global baseline ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_workspace_region_concurrency_enabled(true);

    constexpr int kN = 4;
    constexpr int kSpins = 400'000; // ~body work per agent

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
                    Evaluator::workspace_region_key_from_name(std::format("agent-define-{}", i));
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

    // Warmup
    (void)run_global();
    (void)run_region();

    const double t_global = run_global();
    const double t_region = run_region();
    const double speedup = t_global / (t_region > 1e-9 ? t_region : 1e-9);
    std::println("  global={:.4f}s  region={:.4f}s  speedup={:.2f}×", t_global, t_region, speedup);
    // Allow some noise on loaded CI: require ≥1.3× with aspirational 1.5× note.
    // AC6 text says ≥1.5×; if flaky under heavy load, still require clear win.
    CHECK(speedup >= 1.5, "region path ≥1.5× throughput vs global unique (N=4)");
}

} // namespace

int run_test_workspace_region_concurrency_2121() {
    ac1_source_docs();
    ac2_disjoint_regions_not_global_unique();
    ac3_global_and_fallback();
    ac4_mixed_stress();
    ac5_query_schema();
    ac6_throughput_speedup();

    std::println("\n=== test_workspace_region_concurrency_2121: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_workspace_region_concurrency_2121();
}
#endif
