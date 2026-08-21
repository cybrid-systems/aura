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
#include "compiler/typed_mutation_audit.h"
#include "compiler/mutation_concurrency_health.hh"
#include "compiler/mutation_hold_budget.h"

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

// ── Issue #2990: ConcurrentMutationPolicy (prefer-existing #2121 suite) ──

static std::int64_t href_ws(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:workspace-concurrency-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac2990_1_single_writer_default() {
    std::println("\n--- #2990 AC1: SingleWriter default (zero try_acquire redirect) ---");
    const auto hh = read_file("src/compiler/workspace_concurrent_policy.hh");
    const auto eix = read_file("src/compiler/evaluator.ixx");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(hh.find("ConcurrentMutationPolicy") != std::string::npos, "AC1: policy header");
    CHECK(hh.find("SingleWriter") != std::string::npos, "AC1: SingleWriter");
    CHECK(hh.find("ScopedParallel") != std::string::npos, "AC1: ScopedParallel");
    CHECK(eix.find("scoped_parallel_enabled") != std::string::npos, "AC1: evaluator getter");
    CHECK(mb.find("scoped_parallel_enabled") != std::string::npos, "AC1: try_acquire gated");
    CHECK(mb.find("#2990") != std::string::npos, "AC1: boundary cites #2990");

    CompilerService cs;
    CHECK(cs.evaluator().concurrent_mutation_policy() ==
              Evaluator::ConcurrentMutationPolicy::SingleWriter,
          "AC1: default SingleWriter");
    CHECK(!cs.evaluator().scoped_parallel_enabled(), "AC1: scoped_parallel off");
    auto p = cs.eval("(workspace:concurrent-mutation-policy)");
    CHECK(p.has_value() && is_int(*p) && as_int(*p) == 0, "AC1: EDSL getter 0");

    // TLS region_key must NOT redirect try_acquire under SingleWriter.
    Evaluator& ev = cs.evaluator();
    ev.set_workspace_region_concurrency_enabled(true);
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "AC1: metrics");
    const auto red0 = m->scoped_parallel_redirect_total.load(std::memory_order_relaxed);
    const auto ser0 = m->single_writer_serialize_total.load(std::memory_order_relaxed);
    Evaluator::note_parallel_task_region_key(0xABCDu);
    bool ok = true;
    auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, 1, &ok);
    CHECK(gr.has_value() && *gr, "AC1: try_acquire still admits");
    Evaluator::clear_parallel_task_region_key();
    CHECK(m->scoped_parallel_redirect_total.load(std::memory_order_relaxed) == red0,
          "AC1: no region redirect under SingleWriter");
    CHECK(m->single_writer_serialize_total.load(std::memory_order_relaxed) > ser0,
          "AC1: serialize counter bumped");
}

static void ac2990_2_scoped_parallel_disjoint() {
    std::println("\n--- #2990 AC2: ScopedParallel disjoint admit ---");
    CompilerService cs;
    CHECK(cs.eval("(workspace:set-concurrent-mutation-policy 1)").has_value(),
          "AC2: opt-in ScopedParallel");
    auto p = cs.eval("(workspace:concurrent-mutation-policy)");
    CHECK(p.has_value() && is_int(*p) && as_int(*p) == 1, "AC2: policy == 1");
    Evaluator& ev = cs.evaluator();
    CHECK(ev.scoped_parallel_enabled(), "AC2: scoped_parallel on");
    ev.set_workspace_region_concurrency_enabled(true);
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    const auto red0 = m->scoped_parallel_redirect_total.load(std::memory_order_relaxed);
    Evaluator::note_parallel_task_region_key(Evaluator::workspace_region_key_from_name("d0"));
    bool ok = true;
    auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, 1, &ok);
    CHECK(gr.has_value() && *gr, "AC2: try_acquire redirects + admits");
    Evaluator::clear_parallel_task_region_key();
    CHECK(m->scoped_parallel_redirect_total.load(std::memory_order_relaxed) > red0,
          "AC2: redirect credited");
}

static void ac2990_3_overlap_fallback() {
    std::println("\n--- #2990 AC3: ScopedParallel overlap → SingleWriter fallback ---");
    CompilerService cs;
    (void)cs.eval("(workspace:set-concurrent-mutation-policy \"scoped-parallel\")");
    Evaluator& ev = cs.evaluator();
    ev.set_workspace_region_concurrency_enabled(true);
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    const auto fb0 = m->scoped_parallel_conflict_fallback_total.load(std::memory_order_relaxed);
    const auto key = Evaluator::workspace_region_key_from_name("same-cone");
    bool ok1 = true;
    auto g1 = Evaluator::MutationBoundaryGuard::try_acquire_for_region(ev, key, 1, &ok1);
    CHECK(g1.has_value() && *g1, "AC3: first region admit");
    (*g1).reset(); // release before second acquire on same thread
    bool ok2 = true;
    auto g2 = Evaluator::MutationBoundaryGuard::try_acquire_for_region(ev, key, 1, &ok2);
    CHECK(g2.has_value() && *g2, "AC3: overlap admits via SingleWriter fallback");
    CHECK(m->scoped_parallel_conflict_fallback_total.load(std::memory_order_relaxed) > fb0,
          "AC3: conflict-fallback credited");
}

static void ac2990_4_stats_and_health() {
    std::println("\n--- #2990 AC4: query:workspace-concurrency-stats + #2985/#2976 ---");
    CompilerService cs;
    CHECK(href_ws(cs, "schema-2990") == 2990, "AC4: schema-2990");
    CHECK(href_ws(cs, "issue-2990") == 2990, "AC4: issue-2990");
    CHECK(href_ws(cs, "policy-single-writer-default-wired") == 1, "AC4: default wired");
    CHECK(href_ws(cs, "health-admit-wired") == 1, "AC4: #2985 health wired (not reimplemented)");
    CHECK(href_ws(cs, "agent-scope-policy-wired") == 1, "AC4: #2976 wired");
    CHECK(href_ws(cs, "policy") == 0, "AC4: default policy 0");
    (void)cs.eval("(workspace:set-concurrent-mutation-policy 1)");
    CHECK(href_ws(cs, "policy") == 1, "AC4: policy 1 after opt-in");
    CHECK(href_ws(cs, "scoped-parallel-opt-in-total") >= 1, "AC4: opt-in total");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mb.find("maybe_reject_mutation_concurrency_health") != std::string::npos,
          "AC4: health admit still called (no duplicate throttle)");
}

static std::int64_t href_health(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:mutation-concurrency-health\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac3039_1_production_overlap_hard_reject() {
    std::println("\n--- #3039 AC1: production ScopedParallel overlap → hard reject ---");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    aura::compiler::reset_mutation_concurrency_health_admit_for_test();
    aura::compiler::MutationConcurrencyHealthSnapshot clean;
    aura::compiler::set_mutation_concurrency_health_admit_snapshot_for_test(clean);
    apply_production_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(workspace:set-concurrent-mutation-policy 1)").has_value(),
          "3039 AC1: opt-in ScopedParallel");
    Evaluator& ev = cs.evaluator();
    ev.set_workspace_region_concurrency_enabled(true);
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "3039 AC1: metrics");
    const auto fb0 = m->scoped_parallel_conflict_fallback_total.load(std::memory_order_relaxed);
    const auto hard0 = aura::compiler::mutation_region_overlap_hard_reject_total_v_read();
    const auto key = Evaluator::workspace_region_key_from_name("overlap-3039");
    bool ok1 = true;
    auto g1 = Evaluator::MutationBoundaryGuard::try_acquire_for_region(ev, key, 1, &ok1);
    CHECK(g1.has_value() && *g1, "3039 AC1: first region admit");
    if (g1.has_value())
        (*g1).reset();
    bool ok2 = true;
    auto g2 = Evaluator::MutationBoundaryGuard::try_acquire_for_region(ev, key, 1, &ok2);
    CHECK(!g2.has_value(), "3039 AC1: overlap hard-rejects (no admit)");
    CHECK(g2.error().message.find("region-overlap") != std::string::npos,
          "3039 AC1: structured region-overlap reason");
    CHECK(aura::compiler::mutation_region_overlap_hard_reject_total_v_read() > hard0,
          "3039 AC1: hard-reject counter bumps");
    CHECK(aura::compiler::mutation_region_overlap_last_reason_v_read() == 1,
          "3039 AC1: last-reason region-overlap");
    CHECK(m->scoped_parallel_conflict_fallback_total.load(std::memory_order_relaxed) == fb0,
          "3039 AC1: no GlobalExclusive fallback");
    apply_dev_audit_defaults();
    aura::compiler::reset_mutation_concurrency_health_admit_for_test();
}

static void ac3039_2_production_disjoint_proceeds() {
    std::println("\n--- #3039 AC2: production ScopedParallel disjoint → RegionExclusive ---");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    aura::compiler::reset_mutation_concurrency_health_admit_for_test();
    aura::compiler::MutationConcurrencyHealthSnapshot clean;
    aura::compiler::set_mutation_concurrency_health_admit_snapshot_for_test(clean);
    apply_production_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(workspace:set-concurrent-mutation-policy \"scoped-parallel\")").has_value(),
          "3039 AC2: opt-in");
    Evaluator& ev = cs.evaluator();
    ev.set_workspace_region_concurrency_enabled(true);
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    const auto hard0 = aura::compiler::mutation_region_overlap_hard_reject_total_v_read();
    const auto admit0 = m->scoped_parallel_admit_total.load(std::memory_order_relaxed);
    const auto k0 = Evaluator::workspace_region_key_from_name("d-a-3039");
    const auto k1 = Evaluator::workspace_region_key_from_name("d-b-3039");
    bool ok1 = true;
    auto g1 = Evaluator::MutationBoundaryGuard::try_acquire_for_region(ev, k0, 1, &ok1);
    CHECK(g1.has_value() && *g1, "3039 AC2: first disjoint admit");
    if (g1.has_value())
        (*g1).reset();
    bool ok2 = true;
    auto g2 = Evaluator::MutationBoundaryGuard::try_acquire_for_region(ev, k1, 1, &ok2);
    CHECK(g2.has_value() && *g2, "3039 AC2: second disjoint admit");
    CHECK(aura::compiler::mutation_region_overlap_hard_reject_total_v_read() == hard0,
          "3039 AC2: no hard-reject on disjoint");
    CHECK(m->scoped_parallel_admit_total.load(std::memory_order_relaxed) > admit0,
          "3039 AC2: ScopedParallel admit credited");
    apply_dev_audit_defaults();
    aura::compiler::reset_mutation_concurrency_health_admit_for_test();
}

static void ac3039_3_soft_observe_only() {
    std::println("\n--- #3039 AC3: Soft overlap observe-only (no hard-reject store) ---");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    apply_dev_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(workspace:set-concurrent-mutation-policy 1)").has_value(), "3039 AC3: opt-in");
    Evaluator& ev = cs.evaluator();
    ev.set_workspace_region_concurrency_enabled(true);
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    const auto hard0 = aura::compiler::mutation_region_overlap_hard_reject_total_v_read();
    const auto obs0 = aura::compiler::mutation_region_overlap_reject_total_v_read();
    const auto fb0 = m->scoped_parallel_conflict_fallback_total.load(std::memory_order_relaxed);
    const auto key = Evaluator::workspace_region_key_from_name("soft-3039");
    bool ok1 = true;
    auto g1 = Evaluator::MutationBoundaryGuard::try_acquire_for_region(ev, key, 1, &ok1);
    CHECK(g1.has_value() && *g1, "3039 AC3: first admit");
    (*g1).reset();
    bool ok2 = true;
    auto g2 = Evaluator::MutationBoundaryGuard::try_acquire_for_region(ev, key, 1, &ok2);
    CHECK(g2.has_value() && *g2, "3039 AC3: Soft still degrades/allows");
    CHECK(aura::compiler::mutation_region_overlap_hard_reject_total_v_read() == hard0,
          "3039 AC3: zero extra hard-reject stores");
    CHECK(aura::compiler::mutation_region_overlap_reject_total_v_read() > obs0,
          "3039 AC3: existing overlap metric still bumps");
    CHECK(m->scoped_parallel_conflict_fallback_total.load(std::memory_order_relaxed) > fb0,
          "3039 AC3: Soft may still degrade");
}

static void ac3039_4_schema() {
    std::println("\n--- #3039 AC4: counter + posture keys ---");
    CompilerService cs;
    CHECK(href_ws(cs, "schema-3039") == 3039, "3039 AC4: schema-3039");
    CHECK(href_ws(cs, "issue-3039") == 3039, "3039 AC4: issue-3039");
    CHECK(href_ws(cs, "mutation-region-overlap-hard-reject-wired") == 1, "3039 AC4: wired");
    CHECK(href_ws(cs, "region-overlap-hard-reject-wired") == 1, "3039 AC4: alias wired");
    CHECK(href_ws(cs, "mutation-region-overlap-hard-reject-total") >= 0, "3039 AC4: total key");
    CHECK(href_ws(cs, "schema-2990") == 2990, "3039 AC4: schema-2990 preserved");
    const auto h = href_health(cs, "schema-3039");
    if (h >= 0)
        CHECK(h == 3039, "3039 AC4: health schema-3039");
    else
        CHECK(true, "3039 AC4: light-link skip health");
}

static void ac3039_5_linter_and_suite() {
    std::println("\n--- #3039 AC5/AC6: extend #2990 suite + linter ---");
    const auto t = read_file("tests/compiler/test_workspace_region_concurrency.cpp");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_scoped_parallel_overlap_hard_reject_3039.py");
    const auto lint2990 =
        read_file("scripts/coverage/checks/check_workspace_concurrent_policy_2990.py");
    CHECK(t.find("ac3039_1_production_overlap_hard_reject") != std::string::npos, "3039 AC5: AC1");
    CHECK(t.find("ac2990_3_overlap_fallback") != std::string::npos, "3039 AC5: #2990 Soft kept");
    CHECK(!lint.empty() && lint.find("3039") != std::string::npos, "3039 AC6: successor linter");
    CHECK(lint2990.find("3039") != std::string::npos, "3039 AC6: 2990 linter updated");
    CHECK(build.find("check_scoped_parallel_overlap_hard_reject_3039") != std::string::npos,
          "3039 AC6: build.py");
    CHECK(read_file("docs/design/3039-scoped-parallel-overlap-hard-reject.md").empty(),
          "3039 AC5: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_3039.cpp").empty(), "3039 AC5: no invent test");
}

static void ac2990_5_throughput_and_linter() {
    std::println("\n--- #2990 AC5: source-cite + linter + no invented test ---");
    const auto t = read_file("tests/compiler/test_workspace_region_concurrency.cpp");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_workspace_concurrent_policy_2990.py");
    CHECK(t.find("ac2990_1_single_writer_default") != std::string::npos, "AC5: AC1 test");
    CHECK(t.find("ac2990_3_overlap_fallback") != std::string::npos, "AC5: AC3 test");
    CHECK(build.find("check_workspace_concurrent_policy_2990") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(read_file("docs/design/2990-workspace-concurrent-policy.md").empty(),
          "AC5: no docs/design/2990-* per #1655");
}

} // namespace

int run_test_workspace_region_concurrency() {
    ac1_source_docs();
    ac2_disjoint_regions_not_global_unique();
    ac3_global_and_fallback();
    ac4_mixed_stress();
    ac5_query_schema();
    ac6_throughput_speedup();

    std::println("\n=== Issue #2990: ConcurrentMutationPolicy ===");
    ac2990_1_single_writer_default();
    ac2990_2_scoped_parallel_disjoint();
    ac2990_3_overlap_fallback();
    ac2990_4_stats_and_health();
    ac2990_5_throughput_and_linter();
    std::println("\n=== Issue #3039: ScopedParallel overlap production hard-reject ===");
    ac3039_1_production_overlap_hard_reject();
    ac3039_2_production_disjoint_proceeds();
    ac3039_3_soft_observe_only();
    ac3039_4_schema();
    ac3039_5_linter_and_suite();

    std::println("\n=== test_workspace_region_concurrency: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_workspace_region_concurrency();
}
#endif
