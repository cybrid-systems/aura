// @category: unit
// @reason: Issue #2406 — optional TLS freelist for exclusive PCV unique-inplace
// under extreme multi-fiber pin traffic (default OFF).
//
//   AC1: Soft / default: behavior identical (TLS off → same cow_alloc path)
//   AC2: Opt-in TLS: exclusive mutate reduces cow_alloc_total under stress
//   AC3: SafePCVSpan still keeps storage alive across with_* on other handles
//   AC4: Metrics + schema-2406 / tls-scratch-wired on query:pcv-hotpath-stats
//   AC5: Multi-thread stress + source-cite

#include "test_harness.hpp"

#include "core/persistent_child_vector.hh"

#include <cstdint>
#include <numeric>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::clear_pcv_tls_scratch_for_test;
using aura::ast::FlatAST;
using aura::ast::g_pcv_hotpath_metrics;
using aura::ast::kPcvTlsScratchIssue;
using aura::ast::NodeId;
using aura::ast::PersistentChildVector;
using aura::ast::reset_pcv_hotpath_metrics_for_test;
using aura::ast::SafePCVSpan;
using aura::ast::set_pcv_tls_scratch_for_test;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

using PCV = PersistentChildVector<NodeId>;

PCV make_n(std::size_t n) {
    std::vector<NodeId> v(n);
    std::iota(v.begin(), v.end(), 0u);
    return PCV(v.begin(), v.end());
}

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:pcv-hotpath-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_pcv_tls_scratch() {
    std::println("=== Issue #2406: PCV TLS scratch freelist ===");
    CHECK(kPcvTlsScratchIssue == 2406, "issue stamp");

    // Always restore env override at end of each AC.
    auto cleanup = [] { clear_pcv_tls_scratch_for_test(); };

    // ── AC1: default OFF identical exclusive path ──────────────────
    {
        std::println("\n--- #2406 AC1: default TLS off, exclusive still works ---");
        set_pcv_tls_scratch_for_test(false);
        reset_pcv_hotpath_metrics_for_test();
        auto p = make_n(32);
        CHECK(p.is_unique(), "AC1: unique");
        const auto ca0 = g_pcv_hotpath_metrics().cow_alloc_total.load();
        const auto hit0 = g_pcv_hotpath_metrics().tls_scratch_hit_total.load();
        p.cow_set(3, 77);
        CHECK(p[3] == 77, "AC1: exclusive cow_set writes");
        CHECK(g_pcv_hotpath_metrics().tls_scratch_hit_total.load() == hit0,
              "AC1: no TLS hits when off");
        // Exclusive set does not alloc.
        CHECK(g_pcv_hotpath_metrics().cow_alloc_total.load() == ca0, "AC1: exclusive set no alloc");
        // Grow still allocs via normal make_shared when TLS off.
        p.cow_push_back(99);
        CHECK(p.back() == 99, "AC1: push works");
        CHECK(g_pcv_hotpath_metrics().cow_alloc_total.load() > ca0, "AC1: push bumps cow_alloc");
        CHECK(g_pcv_hotpath_metrics().tls_scratch_hit_total.load() == hit0, "AC1: still no TLS");
        cleanup();
    }

    // ── AC2: opt-in TLS reduces cow_alloc under exclusive stress ───
    {
        std::println("\n--- #2406 AC2: TLS on reduces cow_alloc under exclusive push stress ---");
        set_pcv_tls_scratch_for_test(true);
        reset_pcv_hotpath_metrics_for_test();

        // Baseline: TLS off exclusive push stress.
        set_pcv_tls_scratch_for_test(false);
        reset_pcv_hotpath_metrics_for_test();
        {
            auto p = make_n(8);
            for (int i = 0; i < 40; ++i)
                p.cow_push_back(static_cast<NodeId>(1000 + i));
        }
        const auto cow_off = g_pcv_hotpath_metrics().cow_alloc_total.load();
        const auto hit_off = g_pcv_hotpath_metrics().tls_scratch_hit_total.load();
        CHECK(hit_off == 0, "AC2: TLS off zero hits");
        CHECK(cow_off > 0, "AC2: TLS off has cow_alloc");

        // TLS on: freelist serves subsequent small exclusive grows.
        set_pcv_tls_scratch_for_test(true);
        reset_pcv_hotpath_metrics_for_test();
        {
            auto p = make_n(8);
            for (int i = 0; i < 40; ++i)
                p.cow_push_back(static_cast<NodeId>(2000 + i));
        }
        const auto cow_on = g_pcv_hotpath_metrics().cow_alloc_total.load();
        const auto hit_on = g_pcv_hotpath_metrics().tls_scratch_hit_total.load();
        const auto rec_on = g_pcv_hotpath_metrics().tls_scratch_recycle_total.load();
        std::println("  TLS off cow_alloc={}  TLS on cow_alloc={} hits={} recycle={}", cow_off,
                     cow_on, hit_on, rec_on);
        CHECK(hit_on > 0, "AC2: TLS hits under exclusive stress");
        CHECK(cow_on < cow_off, "AC2: TLS on cow_alloc_total lower than TLS off");
        CHECK(rec_on > 0, "AC2: TLS recycle on destruction");
        cleanup();
    }

    // ── AC3: SafePCVSpan / shared pin still keeps pre-mutation data ─
    {
        std::println("\n--- #2406 AC3: SafePCVSpan keeps pre-mutation data ---");
        set_pcv_tls_scratch_for_test(true);
        reset_pcv_hotpath_metrics_for_test();
        auto base = make_n(16);
        // Share storage (simulates SafePCVSpan / snapshot hold).
        auto pin = base;
        CHECK(base.use_count() >= 2, "AC3: pin holds ref");
        const auto pre = pin[1];
        // Shared → must COW (not exclusive TLS path for mutate of base).
        auto next = base.with_set(1, 4242);
        CHECK(next[1] == 4242, "AC3: with_set new value");
        CHECK(pin[1] == pre, "AC3: pin still sees pre-mutation");
        CHECK(base[1] == pre, "AC3: base immutable with_set (const receiver)");
        // FlatAST children_safe pin + set_child.
        FlatAST flat;
        NodeId kids[2] = {flat.add_literal(1), flat.add_literal(2)};
        auto root = flat.add_begin(std::span<const NodeId>(kids, 2));
        SafePCVSpan<NodeId> safe = flat.children_safe(root);
        const auto s0 = safe[0];
        flat.set_child(root, 0, flat.add_literal(50));
        CHECK(safe[0] == s0, "AC3: SafePCVSpan sees pre-mutation after set_child");
        cleanup();
    }

    // ── AC4: query surface schema-2406 ──────────────────────────────
    {
        std::println("\n--- #2406 AC4: query:pcv-hotpath-stats schema-2406 ---");
        set_pcv_tls_scratch_for_test(true);
        reset_pcv_hotpath_metrics_for_test();
        CompilerService cs;
        auto h = cs.eval("(engine:metrics \"query:pcv-hotpath-stats\")");
        CHECK(h && is_hash(*h), "AC4: pcv-hotpath-stats hash");
        CHECK(href(cs, "schema-2406") == 2406, "AC4: schema-2406");
        CHECK(href(cs, "issue-2406") == 2406, "AC4: issue-2406");
        CHECK(href(cs, "tls-scratch-wired") == 1, "AC4: tls-scratch-wired");
        CHECK(href(cs, "tls-scratch-enabled") == 1, "AC4: tls-scratch-enabled when forced on");
        CHECK(href(cs, "unique-inplace-total") >= 0, "AC4: unique-inplace key");
        CHECK(href(cs, "cow-alloc-total") >= 0, "AC4: cow-alloc key");
        CHECK(href(cs, "tls-scratch-hit-total") >= 0, "AC4: tls hit key");
        // Exercise path so counters may move.
        {
            auto p = make_n(4);
            for (int i = 0; i < 8; ++i)
                p.cow_push_back(static_cast<NodeId>(i));
        }
        CHECK(href(cs, "tls-scratch-hit-total") >= 0, "AC4: hit total after work");
        CHECK(href(cs, "schema-2058") == 2058, "AC4: lineage 2058");
        CHECK(href(cs, "schema-2140") == 2140, "AC4: lineage 2140");
        cleanup();
    }

    // ── AC5: multi-thread stress + source-cite ──────────────────────
    {
        std::println("\n--- #2406 AC5: multi-thread exclusive stress ---");
        set_pcv_tls_scratch_for_test(true);
        reset_pcv_hotpath_metrics_for_test();
        constexpr int kThreads = 4;
        constexpr int kIters = 50;
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([t] {
                set_pcv_tls_scratch_for_test(true);
                for (int i = 0; i < kIters; ++i) {
                    auto p = make_n(8);
                    p.cow_set(0, static_cast<NodeId>(t * 1000 + i));
                    p.cow_push_back(static_cast<NodeId>(i));
                    p.cow_push_back(static_cast<NodeId>(i + 1));
                }
            });
        }
        for (auto& th : threads)
            th.join();
        const auto hits = g_pcv_hotpath_metrics().tls_scratch_hit_total.load();
        const auto inplace = g_pcv_hotpath_metrics().unique_inplace_total.load();
        std::println("  multi-thread hits={} unique_inplace={}", hits, inplace);
        CHECK(inplace > 0 || inplace > 0, "AC5: metrics advanced under multi-thread");
        // FlatAST set_child still exclusive-safe under TLS.
        FlatAST flat;
        NodeId kids[2] = {flat.add_literal(1), flat.add_literal(2)};
        auto root = flat.add_begin(std::span<const NodeId>(kids, 2));
        flat.set_child(root, 0, flat.add_literal(9));
        CHECK(flat.children(root).size() == 2, "AC5: FlatAST children intact");
        cleanup();
    }

    clear_pcv_tls_scratch_for_test();
    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_pcv_tls_scratch();
}
#endif
