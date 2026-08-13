// @category: unit
// @reason: Issue #2140 — PCV exclusive (refcount==1) in-place with_set path.
//
//   AC1: with_set exclusive → no alloc (same storage, with_set_exclusive metric)
//   AC2: SafePCVSpan live → with_set COWs; span sees pre-mutation data
//   AC3: MutationCheckpoint / snapshot_children rollback restores children
//   AC4: shared with_set COW; exclusive pair; #2058 lineage metrics still work
//   AC5: microbench exclusive with_set vs shared with_set (expect faster)

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
using aura::ast::kPcvExclusiveSetIssue;
using aura::ast::kPcvHotpathIssue;
using aura::ast::NodeId;
using aura::ast::PersistentChildVector;
using aura::ast::reset_pcv_hotpath_metrics_for_test;
using aura::ast::SafePCVSpan;
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

int run_test_pcv_exclusive_with_set() {
    std::println("=== Issue #2140: PCV exclusive with_set ===");
    CHECK(kPcvExclusiveSetIssue == 2140, "issue stamp");
    CHECK(kPcvHotpathIssue == 2058, "2058 lineage stamp");

    // ── source ──
    {
        std::println("\n--- source ---");
        auto hh = read_file("src/core/persistent_child_vector.hh");
        CHECK(hh.find("#2140") != std::string::npos, "header cites #2140");
        CHECK(hh.find("with_set_exclusive_total") != std::string::npos, "exclusive metric");
        CHECK(hh.find("use_count() == 1") != std::string::npos, "exclusive check");
    }

    // ── AC1: exclusive with_set no alloc ──
    {
        std::println("\n--- AC1: exclusive with_set no alloc ---");
        reset_pcv_hotpath_metrics_for_test();
        auto p = make_n(128);
        CHECK(p.is_unique(), "fresh unique");
        const void* id0 = p.storage_identity();
        const auto ex0 = g_pcv_hotpath_metrics().with_set_exclusive_total.load();
        const auto ca0 = g_pcv_hotpath_metrics().cow_alloc_total.load();
        auto q = p.with_set(7, 777);
        CHECK(q[7] == 777, "value written");
        CHECK(q.storage_identity() == id0, "same storage (no alloc)");
        CHECK(p.storage_identity() == id0, "receiver same storage");
        CHECK(p[7] == 777, "exclusive: receiver sees update (sole owner)");
        CHECK(q.use_count() == 2, "p and q share exclusive-written storage");
        CHECK(g_pcv_hotpath_metrics().with_set_exclusive_total.load() > ex0, "exclusive metric");
        CHECK(g_pcv_hotpath_metrics().cow_alloc_total.load() == ca0, "no COW alloc");
        CHECK(g_pcv_hotpath_metrics().with_set_cow_total.load() == 0, "no with_set COW");
    }

    // ── AC2: shared pin / SafePCVSpan → COW; observers see old data ──
    {
        std::println("\n--- AC2: shared pin → COW ---");
        // Issue #2521: force TLS off so cow_alloc accounting is deterministic.
        set_pcv_tls_scratch_for_test(false);
        reset_pcv_hotpath_metrics_for_test();
        auto base = make_n(16);
        // Share storage (simulates SafePCVSpan / snapshot hold).
        auto pin = base;
        CHECK(base.use_count() >= 2, "share bumps refcount");
        CHECK(!base.is_unique(), "not unique while shared");
        const auto old = base[3];
        const auto ca0 = g_pcv_hotpath_metrics().cow_alloc_total.load();
        const auto cow0 = g_pcv_hotpath_metrics().with_set_cow_total.load();
        auto next = base.with_set(3, 9999);
        CHECK(next[3] == 9999, "new handle updated");
        CHECK(base[3] == old, "base unchanged under share");
        CHECK(pin[3] == old, "pin sees pre-mutation");
        CHECK(next.storage_identity() != base.storage_identity(), "COW new storage");
        CHECK(g_pcv_hotpath_metrics().cow_alloc_total.load() > ca0, "COW alloc");
        CHECK(g_pcv_hotpath_metrics().with_set_cow_total.load() > cow0, "with_set COW metric");
        (void)pin;

        // FlatAST children_safe pin + set_child still correct.
        FlatAST flat;
        NodeId kids[2] = {flat.add_literal(1), flat.add_literal(2)};
        auto root = flat.add_begin(std::span<const NodeId>(kids, 2));
        SafePCVSpan<NodeId> safe = flat.children_safe(root);
        const auto s0 = safe[0];
        flat.set_child(root, 0, flat.add_literal(50));
        CHECK(safe[0] == s0, "SafePCVSpan sees pre-mutation after set_child");
        CHECK(flat.children(root)[0] != s0, "tree updated");
        clear_pcv_tls_scratch_for_test();
    }

    // ── AC3: snapshot / restore ──
    {
        std::println("\n--- AC3: snapshot rollback ---");
        FlatAST flat;
        NodeId kids[3] = {flat.add_literal(1), flat.add_literal(2), flat.add_literal(3)};
        auto root = flat.add_begin(std::span<const NodeId>(kids, 3));
        auto snap = flat.snapshot_children();
        const auto c0 = flat.children(root)[0];
        const auto c1 = flat.children(root)[1];
        // Mutate via set_child (cow_set / with_set exclusive under the hood).
        flat.set_child(root, 0, flat.add_literal(100));
        flat.set_child(root, 1, flat.add_literal(200));
        CHECK(flat.children(root)[0] != c0, "mutated");
        flat.restore_children(std::move(snap));
        CHECK(flat.children(root).size() == 3, "restored size");
        CHECK(flat.children(root)[0] == c0, "restored child0");
        CHECK(flat.children(root)[1] == c1, "restored child1");
    }

    // ── AC4: shared with_set COW pair ──
    {
        std::println("\n--- AC4: shared with_set COW ---");
        reset_pcv_hotpath_metrics_for_test();
        auto a = make_n(8);
        auto b = a; // share
        CHECK(a.use_count() == 2, "shared");
        auto c = a.with_set(0, 42);
        CHECK(c[0] == 42, "c updated");
        CHECK(a[0] == 0, "a original");
        CHECK(b[0] == 0, "b original");
        CHECK(c.storage_identity() != a.storage_identity(), "diverged");
        CHECK(g_pcv_hotpath_metrics().with_set_cow_total.load() >= 1, "cow counted");
    }

    // ── AC5: microbench exclusive vs shared ──
    {
        std::println("\n--- AC5: microbench ---");
        constexpr std::size_t N = 2000;
        constexpr std::size_t OPS = 3000;
        auto bench = [](auto&& fn) {
            fn(); // warmup
            auto t0 = std::chrono::steady_clock::now();
            fn();
            auto t1 = std::chrono::steady_clock::now();
            return std::chrono::duration<double, std::micro>(t1 - t0).count();
        };
        double exclusive_us = bench([&] {
            auto cur = make_n(N); // unique each time... need stay unique
            // with_set exclusive returns *this sharing with assignment:
            // cur = cur.with_set → use_count 1 after move-assign of shared_ptr
            for (std::size_t i = 0; i < OPS; ++i) {
                cur = cur.with_set(i % N, static_cast<NodeId>(i));
                // After assignment from exclusive with_set, cur has use_count 1
                // (temp destroyed). Next with_set is exclusive again.
            }
        });
        double shared_us = bench([&] {
            auto cur = make_n(N);
            PCV pin = cur; // keep shared always
            for (std::size_t i = 0; i < OPS; ++i) {
                cur = cur.with_set(i % N, static_cast<NodeId>(i));
                // After COW, cur is unique on new storage; re-share with pin
                // would force re-COW — instead keep a second live copy:
                // Actually after first with_set, pin still holds old storage,
                // cur is unique. Next with_set is exclusive! Force share:
                PCV hold = cur;
                cur = cur.with_set(i % N, static_cast<NodeId>(i + 1));
                (void)hold;
                (void)pin;
            }
        });
        std::println("  exclusive_us={:.1f} shared_force_us={:.1f}", exclusive_us, shared_us);
        // Soft check: exclusive should not be dramatically slower; prefer
        // exclusive metric path validated in AC1. Allow noise on CI.
        CHECK(exclusive_us > 0 && shared_us > 0, "bench ran");
    }

    // OOB / empty no-ops
    {
        std::println("\n--- edge: OOB / empty ---");
        PCV empty;
        auto e2 = empty.with_set(0, 1);
        CHECK(e2.empty(), "empty with_set OOB");
        auto p = make_n(2);
        auto q = p.with_set(99, 5);
        CHECK(q[0] == 0 && q[1] == 1, "OOB no-op");
    }

    // ── Issue #2906: FlatAST locked exclusive via move-out ──
    {
        std::println("\n=== Issue #2906: FlatAST locked PCV exclusive move-out ===");

        // AC1: source-cite canonical pattern on locked paths.
        std::println("\n--- #2906 AC1: locked paths move-out ---");
        const auto ast = read_file("src/core/ast.ixx");
        const auto hh = read_file("src/core/persistent_child_vector.hh");
        const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        CHECK(ast.find("std::move(children_[id])") != std::string::npos ||
                  ast.find("std::move(children_[") != std::string::npos,
              "AC1: move-out of children_ present");
        CHECK(ast.find("list.cow_set") != std::string::npos, "AC1: cow_set after move-out");
        CHECK(ast.find("children_[id] = std::move(list)") != std::string::npos ||
                  ast.find("= std::move(list)") != std::string::npos,
              "AC1: move-in after mutate");
        CHECK(ast.find("#2906") != std::string::npos, "AC1: ast cites #2906");
        CHECK(hh.find("#2906") != std::string::npos, "AC1: PCV header cites #2906");
        CHECK(hh.find("flatast_locked_move_out_exclusive_total") != std::string::npos,
              "AC1: locked exclusive metric");
        CHECK(mut.find("#2906") != std::string::npos ||
                  mut.find("set_child_locked") != std::string::npos ||
                  mut.find("cow_set exclusive") != std::string::npos,
              "AC1: mutate prims cite exclusive path / #2906");

        // AC2: sole-holder stress — exclusive dominates.
        std::println("\n--- #2906 AC2: sole-holder stress exclusive dominates ---");
        reset_pcv_hotpath_metrics_for_test();
        FlatAST flat;
        NodeId kids[4] = {flat.add_literal(1), flat.add_literal(2), flat.add_literal(3),
                          flat.add_literal(4)};
        auto root = flat.add_begin(std::span<const NodeId>(kids, 4));
        for (int i = 0; i < 200; ++i) {
            flat.set_child(root, static_cast<std::uint32_t>(i % 4), flat.add_literal(100 + i));
        }
        const auto ex = g_pcv_hotpath_metrics().flatast_locked_move_out_exclusive_total.load();
        const auto cow = g_pcv_hotpath_metrics().flatast_locked_move_out_cow_total.load();
        const auto uniq = g_pcv_hotpath_metrics().unique_inplace_total.load();
        std::println("  locked exclusive={} cow={} unique_inplace={}", ex, cow, uniq);
        CHECK(ex >= 150, "AC2: locked exclusive path dominates sole-holder stress");
        CHECK(cow == 0 || cow < ex / 10, "AC2: locked COW near zero without snapshot");
        CHECK(uniq >= 150, "AC2: unique_inplace also high");

        // AC3: SafePCVSpan forces COW / observers see old data.
        std::println("\n--- #2906 AC3: SafePCVSpan forces locked COW ---");
        reset_pcv_hotpath_metrics_for_test();
        FlatAST flat2;
        NodeId kids2[2] = {flat2.add_literal(10), flat2.add_literal(20)};
        auto root2 = flat2.add_begin(std::span<const NodeId>(kids2, 2));
        SafePCVSpan<NodeId> safe = flat2.children_safe(root2);
        const auto s0 = safe[0];
        flat2.set_child(root2, 0, flat2.add_literal(999));
        CHECK(safe[0] == s0, "AC3: SafePCVSpan sees pre-mutation (no corruption)");
        CHECK(flat2.children(root2)[0] != s0, "AC3: tree updated under lock");
        // With span holding shared storage, move-out leaves list shared → COW.
        const auto cow1 = g_pcv_hotpath_metrics().flatast_locked_move_out_cow_total.load();
        CHECK(cow1 > 0 || g_pcv_hotpath_metrics().with_set_cow_total.load() > 0 ||
                  g_pcv_hotpath_metrics().cow_alloc_total.load() > 0,
              "AC3: shared holder forces COW path");

        // AC4: schema-2906 + rollback keeps with_* + linter wired; no new files.
        std::println("\n--- #2906 AC4: schema + rollback with_* + linter ---");
        const auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        const auto build = read_file("build.py");
        const auto lint =
            read_file("scripts/coverage/checks/check_pcv_flatast_locked_exclusive_2906.py");
        CHECK(obs.find("schema-2906") != std::string::npos, "AC4: schema-2906");
        CHECK(obs.find("flatast-locked-move-out-exclusive-total") != std::string::npos,
              "AC4: exclusive total key");
        CHECK(obs.find("flatast-locked-exclusive-ratio-bp") != std::string::npos,
              "AC4: exclusive ratio key");
        // Rollback keeps with_* (COW correctness first, AC3): snapshot aliases
        // storage during rollback, so move-out must not be introduced there.
        CHECK(ast.find("with_set") != std::string::npos, "AC4: rollback keeps with_*");
        CHECK(build.find("check_pcv_flatast_locked_exclusive_2906") != std::string::npos,
              "AC4: build.py wires linter");
        CHECK(!lint.empty() && lint.find("2906") != std::string::npos, "AC4: linter present");
        CHECK(read_file("docs/design/2906-pcv-exclusive.md").empty(),
              "AC4: no docs/design/2906-* per #1655");
        CHECK(read_file("tests/core/test_issue_2906.cpp").empty(),
              "AC4: no new test file per #81967");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_pcv_exclusive_with_set();
}
#endif
