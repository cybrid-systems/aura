// Issue #2040 — high-concurrency observability for Guard hold +
// workspace_mtx_ contention (p99 proxy + counters).
//
// AC1: workspace_mtx_acquire / contended / wait_ns counters + hold
//      histogram p99 proxy exist on CompilerMetrics.
// AC2: (engine:metrics \"query:mutation-boundary-hold-stats\") exposes
//      hold p99/max + workspace-mtx-* fields (schema 2040). Reuses the
//      existing stats name (SlimSurface freeze — no new *-stats).
// AC3: Single-thread Guard cycle bumps holds-total + acquire-total;
//      concurrent mutates exercise the path without crash.
// AC4: Uncontended path uses try_lock (cheap relaxed atomics only).

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"

#include <atomic>
#include <chrono>
#include <cstdint>
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
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

std::int64_t href(CompilerService& cs, const char* prim, const char* key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", prim, key));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

} // namespace

int run_test_mutation_contention_2040() {
    std::println("=== Issue #2040: mutation contention + hold p99 observability ===");

    // ── AC1: metrics fields exist ─────────────────────────────────
    {
        std::println("\n--- AC1: CompilerMetrics contention fields ---");
        CompilerMetrics m;
        CHECK(m.workspace_mtx_acquire_total.load() == 0, "acquire starts 0");
        CHECK(m.workspace_mtx_contended_total.load() == 0, "contended starts 0");
        CHECK(m.workspace_mtx_wait_ns_total.load() == 0, "wait_ns starts 0");
        CHECK(m.workspace_mtx_wait_ns_max.load() == 0, "wait_max starts 0");
        CHECK(CompilerMetrics::kMutationBoundaryHoldHistBuckets == 9, "9 hold buckets");
        m.workspace_mtx_acquire_total.fetch_add(1, std::memory_order_relaxed);
        m.workspace_mtx_contended_total.fetch_add(1, std::memory_order_relaxed);
        m.workspace_mtx_wait_ns_total.fetch_add(12'000, std::memory_order_relaxed);
        m.workspace_mtx_wait_ns_max.store(12'000, std::memory_order_relaxed);
        CHECK(m.workspace_mtx_contended_total.load() == 1, "contended +1");
        CHECK(m.workspace_mtx_wait_ns_max.load() == 12'000, "wait max set");
    }

    // ── AC2: hold-stats schema + contention fields ────────────────
    {
        std::println("\n--- AC2: query:mutation-boundary-hold-stats (#2040 fields) ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        {
            bool ok = true;
            Evaluator::MutationBoundaryGuard g(ev, &ok);
            (void)g;
            CHECK(ok, "guard acquired");
        }
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m != nullptr, "metrics wired");
        CHECK(m->mutation_boundary_holds_total.load(std::memory_order_relaxed) >= 1,
              "holds-total moved");
        CHECK(m->workspace_mtx_acquire_total.load(std::memory_order_relaxed) >= 1,
              "acquire-total moved");

        auto hold = cs.eval("(engine:metrics \"query:mutation-boundary-hold-stats\")");
        CHECK(hold && is_hash(*hold), "hold-stats is hash");
        CHECK(href(cs, "query:mutation-boundary-hold-stats", "schema") == 2040, "schema 2040");
        CHECK(href(cs, "query:mutation-boundary-hold-stats", "issue") == 2040, "issue 2040");
        CHECK(href(cs, "query:mutation-boundary-hold-stats", "holds-total") >= 1, "holds-total");
        CHECK(href(cs, "query:mutation-boundary-hold-stats", "avg-hold-us") >= 0, "avg-hold-us");
        CHECK(href(cs, "query:mutation-boundary-hold-stats", "max-hold-us") >= 0, "max-hold-us");
        CHECK(href(cs, "query:mutation-boundary-hold-stats", "p99-hold-us") >= 0, "p99-hold-us");
        CHECK(href(cs, "query:mutation-boundary-hold-stats", "workspace-mtx-acquire-total") >= 1,
              "mtx acquire");
        CHECK(href(cs, "query:mutation-boundary-hold-stats", "workspace-mtx-contended-total") >= 0,
              "mtx contended");
        CHECK(href(cs, "query:mutation-boundary-hold-stats", "workspace-mtx-wait-ns-total") >= 0,
              "wait ns");
        CHECK(href(cs, "query:mutation-boundary-hold-stats", "workspace-mtx-contention-rate-pct") >=
                  0,
              "rate pct");
        CHECK(href(cs, "query:mutation-boundary-hold-stats", "hist-sum") >= 1, "hist-sum");
    }

    // ── AC3: concurrent Guard acquires ────────────────────────────
    {
        std::println("\n--- AC3: concurrent outermost Guards ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto a0 = m->workspace_mtx_acquire_total.load(std::memory_order_relaxed);
        const auto h0 = m->mutation_boundary_holds_total.load(std::memory_order_relaxed);
        constexpr int kThreads = 4;
        constexpr int kIters = 40;
        std::atomic<int> ok_count{0};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&]() {
                for (int i = 0; i < kIters; ++i) {
                    bool ok = true;
                    {
                        Evaluator::MutationBoundaryGuard g(ev, &ok);
                        (void)g;
                        if (i % 7 == 0)
                            std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }
                    if (ok)
                        ok_count.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads)
            th.join();
        const auto a1 = m->workspace_mtx_acquire_total.load(std::memory_order_relaxed);
        const auto h1 = m->mutation_boundary_holds_total.load(std::memory_order_relaxed);
        const auto cont = m->workspace_mtx_contended_total.load(std::memory_order_relaxed);
        std::println("  acquires {}→{} holds {}→{} contended={} ok={}", a0, a1, h0, h1, cont,
                     ok_count.load());
        CHECK(a1 >= a0 + static_cast<std::uint64_t>(kThreads * kIters),
              "acquire-total grew by concurrent outermost Guards");
        CHECK(h1 >= h0 + static_cast<std::uint64_t>(kThreads * kIters),
              "holds-total grew under concurrent load");
        CHECK(ok_count.load() == kThreads * kIters, "all Guards succeeded");
        CHECK(cont >= 0, "contended non-negative");
        auto q = cs.eval("(engine:metrics \"query:mutation-boundary-hold-stats\")");
        CHECK(q && is_hash(*q), "hold-stats after concurrent load");
        CHECK(href(cs, "query:mutation-boundary-hold-stats", "workspace-mtx-acquire-total") >=
                  static_cast<std::int64_t>(a1),
              "query reflects acquires");
    }

    // ── AC4: uncontended try_lock path ────────────────────────────
    {
        std::println("\n--- AC4: uncontended try_lock path ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto c0 = m->workspace_mtx_contended_total.load(std::memory_order_relaxed);
        {
            bool ok = true;
            Evaluator::MutationBoundaryGuard g(ev, &ok);
            (void)g;
            CHECK(ok, "uncontended guard ok");
        }
        const auto c1 = m->workspace_mtx_contended_total.load(std::memory_order_relaxed);
        const auto a = m->workspace_mtx_acquire_total.load(std::memory_order_relaxed);
        CHECK(a >= 1, "uncontended still counts acquire");
        CHECK(c1 == c0, "uncontended path does not bump contended");
    }

    std::println("\n=== Issue #2040 results: {} passed, {} failed ===", ::aura::test::g_passed,
                 ::aura::test::g_failed);
    return ::aura::test::g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_mutation_contention_2040();
}
#endif
