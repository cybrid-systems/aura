// @category: unit
// @reason: Issue #2058 — PersistentChildVector unique-ownership hot path
// for AI multi-round structural mutation (avoid atomic COW when sole owner).
//
//   AC1: cow_set unique → in-place (no new storage, use_count stays 1)
//   AC2: cow_set shared → allocates; original storage unchanged
//   AC3: FlatAST set_child unique path hits unique_inplace metric
//   AC4: snapshot/rollback still correct after unique + shared mixes
//   AC5: microbench: move+cow_set ≥20% faster than copy+with_set (5k × N)
//   AC6: metrics + docs stamp (kPcvHotpathIssue == 2058)

#include "test_harness.hpp"

#include "core/persistent_child_vector.hh"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <numeric>
#include <print>
#include <string>
#include <vector>

import std;
import aura.core.ast;

namespace {

using aura::ast::clear_pcv_tls_scratch_for_test;
using aura::ast::FlatAST;
using aura::ast::g_pcv_hotpath_metrics;
using aura::ast::kPcvHotpathIssue;
using aura::ast::NodeId;
using aura::ast::PersistentChildVector;
using aura::ast::reset_pcv_hotpath_metrics_for_test;
using aura::ast::set_pcv_tls_scratch_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

using PCV = PersistentChildVector<NodeId>;

PCV make_n(std::size_t n) {
    std::vector<NodeId> v(n);
    std::iota(v.begin(), v.end(), 0u);
    return PCV(v.begin(), v.end());
}

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

} // namespace

int run_test_pcv_unique_hotpath() {
    std::println("=== Issue #2058: PCV unique hot-path ===");
    CHECK(kPcvHotpathIssue == 2058, "issue stamp");

    // ── AC1: unique cow_set in-place ──
    {
        std::println("\n--- AC1: unique cow_set in-place ---");
        reset_pcv_hotpath_metrics_for_test();
        auto p = make_n(64);
        CHECK(p.is_unique(), "fresh PCV unique");
        const void* id0 = p.storage_identity();
        const auto up0 = g_pcv_hotpath_metrics().unique_inplace_total.load();
        const auto ca0 = g_pcv_hotpath_metrics().cow_alloc_total.load();
        p.cow_set(3, 999);
        CHECK(p[3] == 999, "value written");
        CHECK(p.storage_identity() == id0, "same storage (in-place)");
        CHECK(p.is_unique(), "still unique");
        CHECK(g_pcv_hotpath_metrics().unique_inplace_total.load() > up0, "unique metric");
        CHECK(g_pcv_hotpath_metrics().cow_alloc_total.load() == ca0, "no COW alloc on unique set");
    }

    // ── AC2: shared cow_set allocates; original intact ──
    {
        std::println("\n--- AC2: shared cow_set COW ---");
        // Issue #2521: force TLS off so cow_alloc accounting is deterministic.
        set_pcv_tls_scratch_for_test(false);
        reset_pcv_hotpath_metrics_for_test();
        auto base = make_n(32);
        auto snap = base; // share storage
        CHECK(base.use_count() == 2, "shared after copy");
        CHECK(!base.is_unique(), "not unique when shared");
        const auto ca0 = g_pcv_hotpath_metrics().cow_alloc_total.load();
        base.cow_set(1, 42);
        CHECK(base[1] == 42, "mutated holder sees new value");
        CHECK(snap[1] == 1, "snapshot still original");
        CHECK(base.storage_identity() != snap.storage_identity(), "diverged storage");
        CHECK(g_pcv_hotpath_metrics().cow_alloc_total.load() > ca0, "COW alloc counted");
        CHECK(snap.use_count() == 1, "snap sole owner of old");
        CHECK(base.is_unique(), "base unique on new storage");
        clear_pcv_tls_scratch_for_test();
    }

    // ── AC3: FlatAST set_child unique path ──
    {
        std::println("\n--- AC3: FlatAST set_child unique ---");
        reset_pcv_hotpath_metrics_for_test();
        FlatAST flat;
        NodeId kids[3] = {flat.add_literal(1), flat.add_literal(2), flat.add_literal(3)};
        auto root = flat.add_begin(std::span<const NodeId>(kids, 3));
        CHECK(flat.children(root).size() >= 3, "children present");

        // No external snapshot → set_child should take unique path after move.
        const auto c0 = flat.children(root)[1];
        flat.set_child(root, 1, flat.add_literal(99));
        CHECK(flat.children(root)[1] != c0, "child replaced");
        // Stronger: repeated set_child on same slot should hit unique.
        const auto up1 = g_pcv_hotpath_metrics().unique_inplace_total.load();
        for (int i = 0; i < 10; ++i)
            flat.set_child(root, 0, flat.add_literal(100 + i));
        std::println("  unique_inplace after set_child loop: {} (was {})",
                     g_pcv_hotpath_metrics().unique_inplace_total.load(), up1);
        CHECK(g_pcv_hotpath_metrics().unique_inplace_total.load() > up1,
              "repeated set_child uses unique in-place");
    }

    // ── AC4: snapshot/rollback correctness ──
    {
        std::println("\n--- AC4: snapshot / restore ---");
        FlatAST flat;
        NodeId kids[2] = {flat.add_literal(10), flat.add_literal(20)};
        auto root = flat.add_begin(std::span<const NodeId>(kids, 2));
        auto snap = flat.snapshot_children();
        const auto n0 = flat.children(root).size();
        flat.set_child(root, 0, flat.add_literal(30));
        flat.insert_child(root, 2, flat.add_literal(40));
        CHECK(flat.children(root).size() == n0 + 1, "grew after insert");
        flat.restore_children(std::move(snap));
        CHECK(flat.children(root).size() == n0, "restored size");
        CHECK(flat.children(root).size() >= 2, "restored children present");
    }

    // ── AC5: microbench ≥20% ──
    {
        std::println("\n--- AC5: microbench move+cow_set vs copy+with_set ---");
        // Issue #2521: force TLS off so unique vs shared-COW timing measures
        // pure alloc vs in-place (default-on freelist blurs both paths).
        // Note: exclusive with_set (#2140) is also in-place when unique, so
        // the "shared" baseline must force refcount>1 each step (copy fork).
        set_pcv_tls_scratch_for_test(false);
        constexpr std::size_t N = 5000;
        constexpr std::size_t OPS = 2000;
        auto base = make_n(N);

        auto bench = [](auto&& fn) {
            // warmup
            fn();
            auto t0 = std::chrono::steady_clock::now();
            fn();
            auto t1 = std::chrono::steady_clock::now();
            return std::chrono::duration<double, std::micro>(t1 - t0).count();
        };

        // Forced-share COW: copy then with_set (refcount>1 → always alloc)
        double shared_us = bench([&] {
            PCV cur = base;
            for (std::size_t i = 0; i < OPS; ++i) {
                PCV tmp = cur; // bump refcount → next with_set always COWs
                cur = tmp.with_set(i % N, static_cast<NodeId>(i));
            }
        });

        // Hot path: move-like unique ownership + cow_set
        double hot_us = bench([&] {
            PCV cur = base;
            // Detach from base so cur is unique
            {
                PCV sink = std::move(cur);
                cur = std::move(sink);
            }
            // Ensure unique by cloning once if still shared with base
            if (!cur.is_unique()) {
                PCV clone = cur.with_set(0, cur[0]); // force private
                cur = std::move(clone);
            }
            for (std::size_t i = 0; i < OPS; ++i) {
                cur.cow_set(i % N, static_cast<NodeId>(i));
            }
        });

        // Pure unique bench: start unique (in-place cow_set)
        double unique_us = bench([&] {
            auto cur = make_n(N); // unique
            for (std::size_t i = 0; i < OPS; ++i)
                cur.cow_set(i % N, static_cast<NodeId>(i * 3));
        });

        const double speedup = (shared_us > 0) ? (shared_us / unique_us) : 0.0;
        const double reduction_pct =
            (shared_us > 0) ? (100.0 * (shared_us - unique_us) / shared_us) : 0.0;
        const double hot_reduction =
            (shared_us > 0) ? (100.0 * (shared_us - hot_us) / shared_us) : 0.0;
        std::println("  forced-share with_set: {:.1f} µs ({} ops)", shared_us, OPS);
        std::println("  unique cow_set:        {:.1f} µs  (speedup {:.2f}x, -{:.1f}%)", unique_us,
                     speedup, reduction_pct);
        std::println("  hot move pattern:      {:.1f} µs  (-{:.1f}%)", hot_us, hot_reduction);
        // AC: ≥20% reduction unique/hot vs forced-share COW
        CHECK(reduction_pct >= 20.0 || hot_reduction >= 20.0 || unique_us < shared_us * 0.80 ||
                  hot_us < shared_us * 0.80,
              std::format("≥20% reduction unique vs forced-share (unique -{:.1f}%, hot -{:.1f}%)",
                          reduction_pct, hot_reduction));
        clear_pcv_tls_scratch_for_test();
    }

    // ── AC6: ensure_unique + metrics ──
    {
        std::println("\n--- AC6: ensure_unique ---");
        reset_pcv_hotpath_metrics_for_test();
        auto a = make_n(8);
        auto b = a;
        CHECK(!a.is_unique(), "shared");
        const auto c0 = g_pcv_hotpath_metrics().ensure_unique_clone_total.load();
        CHECK(a.ensure_unique(), "cloned");
        CHECK(a.is_unique(), "now unique");
        CHECK(b.use_count() == 1, "other sole on old");
        CHECK(g_pcv_hotpath_metrics().ensure_unique_clone_total.load() > c0, "clone metric");
        CHECK(!a.ensure_unique(), "second ensure no-op");
    }

    // ── #3491: production unique path zero RMW; Soft/unit notes stay ──
    {
        std::println("\n--- #3491 AC1: unique cow_set / exclusive with_set use AURA_PCV_NOTE ---");
        CHECK(aura::ast::kPcvUniqueZeroAtomicIssue == 3491, "3491 AC1: issue stamp");
        const auto hh = read_file("src/core/persistent_child_vector.hh");
        CHECK(hh.find("kPcvUniqueZeroAtomicIssue = 3491") != std::string::npos, "3491 AC1: stamp");
        CHECK(hh.find("#define AURA_PCV_NOTE") != std::string::npos, "3491 AC1: note macro");
        CHECK(hh.find("AURA_PRODUCTION_PACK") != std::string::npos &&
                  hh.find("AURA_PCV_METRICS") != std::string::npos,
              "3491 AC1: production NDEBUG pack no-ops notes");
        const auto cs = hh.find("void cow_set(");
        const auto cp = hh.find("void cow_push_back(");
        const auto cow =
            (cs != std::string::npos && cp > cs) ? hh.substr(cs, cp - cs) : std::string{};
        CHECK(cow.find("AURA_PCV_NOTE(cow_set_total)") != std::string::npos,
              "3491 AC1: cow_set_total noted");
        CHECK(cow.find("AURA_PCV_NOTE(unique_inplace_total)") != std::string::npos,
              "3491 AC1: unique_inplace noted");
        CHECK(cow.find(".fetch_add(") == std::string::npos,
              "3491 AC1: unique cow_set no raw fetch_add");
        const auto ws = hh.find("PersistentChildVector with_set(");
        const auto wend = hh.find("void cow_set(", ws);
        const auto wwin =
            (ws != std::string::npos && wend > ws) ? hh.substr(ws, wend - ws) : std::string{};
        const auto ex = wwin.find("data_.use_count() == 1");
        const auto sh = wwin.find("tls_pcv_acquire");
        const auto exclusive =
            (ex != std::string::npos && sh > ex) ? wwin.substr(ex, sh - ex) : std::string{};
        CHECK(exclusive.find("AURA_PCV_NOTE(unique_inplace_total)") != std::string::npos,
              "3491 AC1: exclusive with_set noted");
        CHECK(exclusive.find("AURA_PCV_NOTE(with_set_exclusive_total)") != std::string::npos,
              "3491 AC1: with_set_exclusive noted");
        CHECK(exclusive.find(".fetch_add(") == std::string::npos,
              "3491 AC1: exclusive with_set no raw fetch_add");

        std::println("\n--- #3491 AC2: shared TLS-then-heap + Snapshot COW ---");
        CHECK(wwin.find("tls_pcv_acquire") != std::string::npos &&
                  wwin.find("heap_pcv_allocate") != std::string::npos &&
                  wwin.find("tls_pcv_acquire") < wwin.find("heap_pcv_allocate"),
              "3491 AC2: TLS before heap");
        CHECK(cow.find("*this = with_set") != std::string::npos,
              "3491 AC2: shared cow_set delegates");
        const auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("auto kids = std::move(children_[id]);") != std::string::npos,
              "3491 AC5: locked move-out pattern");

        std::println("\n--- #3491 AC3: Soft/unit counters still move ---");
        reset_pcv_hotpath_metrics_for_test();
        auto p = make_n(8);
        const auto up0 = g_pcv_hotpath_metrics().unique_inplace_total.load();
        const auto cs0 = g_pcv_hotpath_metrics().cow_set_total.load();
        p.cow_set(0, 7);
        CHECK(p[0] == 7, "3491 AC3: unique write");
        CHECK(g_pcv_hotpath_metrics().unique_inplace_total.load() > up0,
              "3491 AC3: unique_inplace");
        CHECK(g_pcv_hotpath_metrics().cow_set_total.load() > cs0, "3491 AC3: cow_set_total");
        auto q = p;
        const auto cow0 = g_pcv_hotpath_metrics().with_set_cow_total.load();
        q.cow_set(1, 8);
        CHECK(q[1] == 8 && p[1] == 1, "3491 AC3: shared COW");
        CHECK(g_pcv_hotpath_metrics().with_set_cow_total.load() > cow0, "3491 AC3: with_set_cow");

        std::println("\n--- #3491 AC4/AC5: suite + linter, no invent ---");
        const auto build = read_file("build.py");
        CHECK(build.find("check_pcv_unique_zero_atomic_3491") != std::string::npos,
              "3491 AC4: build.py");
        const auto p3429 = build.find("check_pcv_shared_cow_tls_3429");
        const auto p3491 = build.find("check_pcv_unique_zero_atomic_3491");
        CHECK(p3429 != std::string::npos && p3491 != std::string::npos && p3491 > p3429,
              "3491 AC4: linter AFTER #3429");
        CHECK(hh.find("sizeof(PcvHotpathMetrics) == 136") != std::string::npos,
              "3491 AC5: metrics size unchanged");
        CHECK(hh.find("g_3491_") == std::string::npos, "3491 AC5: no g_3491_*");
        const auto qsrc = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(qsrc.find("schema-3491") == std::string::npos, "3491 AC5: no schema-3491");
        CHECK(qsrc.find("unique-inplace-total") != std::string::npos,
              "3491 AC5: reuse unique-inplace");
        CHECK(read_file("docs/design/3491-pcv-unique-zero-atomic.md").empty(),
              "3491 AC4: no docs/design");
        CHECK(read_file("tests/core/test_issue_3491.cpp").empty(), "3491 AC4: no invent");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_pcv_unique_hotpath();
}
#endif
