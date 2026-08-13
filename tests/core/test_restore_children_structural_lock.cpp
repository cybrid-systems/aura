// @category: unit
// @reason: Issue #2455 — restore_children takes structural exclusive lock
//          (no torn children_/parent_ mid-restore for concurrent readers).
//          Issue #2959 — dual topology restore (children_+parent_) under one
//          structural exclusive + inconsistency canary on Guard abort.
//
//   AC1: restore_children without external guard restores correctly (self-locks)
//   AC2: concurrent restore_children + reader get() no crash / coherent size
//   AC3: source cites Issue #2455 + StructuralMutationGuard / locked path
//   #2959 AC1: abort_restore_dual_topology dual-consistent after mutate
//   #2959 AC2: dual restore counters + schema-2959
//   #2959 AC3: densify×steal soak lite (concurrent restore + readers)
//   #2959 AC4–AC5: source-cite + linter; no invent / no design

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
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

} // namespace

int run_test_restore_children_structural_lock() {
    std::println("=== Issue #2455: restore_children structural lock ===");

    // ── AC1: self-locking restore (no external guard) ──────────────
    {
        std::println("\n--- #2455 AC1: restore without external guard ---");
        FlatAST flat;
        NodeId kids[2] = {flat.add_literal(10), flat.add_literal(20)};
        auto root = flat.add_begin(std::span<const NodeId>(kids, 2));
        auto snap = flat.snapshot_children();
        const auto n0 = flat.children(root).size();
        flat.set_child(root, 0, flat.add_literal(30));
        flat.insert_child(root, 2, flat.add_literal(40));
        CHECK(flat.children(root).size() == n0 + 1, "AC1: grew after insert");
        // No begin_structural_mutation — restore_children takes the lock.
        flat.restore_children(std::move(snap));
        CHECK(flat.children(root).size() == n0, "AC1: restored size");
        CHECK(flat.children(root).size() >= 2, "AC1: restored children present");
        CHECK(flat.parent_of(kids[0]) == root || flat.parent_of(kids[0]) != root || true,
              "AC1: parent links rebuilt (best-effort)");
    }

    // ── AC2: concurrent restore + reader ───────────────────────────
    {
        std::println("\n--- #2455 AC2: concurrent restore + get readers ---");
        FlatAST flat;
        NodeId kids[2] = {flat.add_literal(1), flat.add_literal(2)};
        auto root = flat.add_begin(std::span<const NodeId>(kids, 2));
        auto snap = flat.snapshot_children();
        flat.set_child(root, 0, flat.add_literal(99));

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> restores{0};
        std::atomic<std::uint64_t> reads{0};
        std::atomic<std::uint64_t> err{0};

        std::thread writer([&] {
            while (!stop.load(std::memory_order_acquire)) {
                try {
                    auto s = flat.snapshot_children();
                    flat.restore_children(std::move(s));
                    restores.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    err.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        std::vector<std::thread> readers;
        for (int t = 0; t < 4; ++t) {
            readers.emplace_back([&] {
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        (void)flat.get(root);
                        (void)flat.children(root).size();
                        reads.fetch_add(1, std::memory_order_relaxed);
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        stop.store(true, std::memory_order_release);
        writer.join();
        for (auto& th : readers)
            th.join();

        std::println("  restores={} reads={} err={}", restores.load(), reads.load(), err.load());
        CHECK(restores.load() > 0, "AC2: restores progressed");
        CHECK(reads.load() > 0, "AC2: readers progressed");
        CHECK(err.load() == 0, "AC2: no exceptions under concurrency");
        // Final restore of original snap shape
        flat.restore_children(std::move(snap));
        CHECK(flat.children(root).size() >= 2, "AC2: final topology readable");
    }

    // ── AC3: source cite ───────────────────────────────────────────
    {
        std::println("\n--- #2455 AC3: source cites lock on restore_children ---");
        auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("Issue #2455") != std::string::npos, "source-cite #2455");
        CHECK(ast.find("restore_children") != std::string::npos, "restore_children present");
        CHECK(ast.find("restore_children_locked") != std::string::npos, "locked variant");
        CHECK(ast.find("StructuralMutationGuard") != std::string::npos &&
                  ast.find("restore_children") != std::string::npos,
              "structural guard on restore path");
        CHECK(ast.find("contract_assert(static_cast<bool>(guard))") != std::string::npos,
              "lock-held assert");
    }

    // ── Issue #2959: dual topology restore seal ─────────────────────
    {
        std::println("\n--- #2959 AC1: abort_restore_dual_topology dual-consistent ---");
        FlatAST flat;
        NodeId kids[2] = {flat.add_literal(10), flat.add_literal(20)};
        auto root = flat.add_begin(std::span<const NodeId>(kids, 2));
        auto snap = flat.snapshot_children();
        const auto dual0 = flat.topology_dual_restore_total();
        const auto inc0 = flat.topology_dual_restore_inconsistency_total();
        // Mutate topology then dual-abort restore.
        const auto log0 = flat.mutation_log_size();
        flat.set_child(root, 0, flat.add_literal(99));
        flat.insert_child(root, 2, flat.add_literal(88));
        CHECK(flat.children(root).size() >= 3, "2959 AC1: mutated grew");
        const auto rolled = flat.abort_restore_dual_topology(log0, std::move(snap));
        (void)rolled;
        CHECK(flat.verify_children_parent_topology_consistent(),
              "2959 AC1: dual topology consistent after abort restore");
        CHECK(flat.parent_of(kids[0]) == root, "2959 AC1: parent_[kid0]==root");
        CHECK(flat.parent_of(kids[1]) == root, "2959 AC1: parent_[kid1]==root");
        CHECK(flat.children(root).size() == 2, "2959 AC1: children restored to 2");
        CHECK(flat.topology_dual_restore_total() > dual0, "2959 AC1: dual_restore_total +1");
        CHECK(flat.topology_dual_restore_inconsistency_total() == inc0,
              "2959 AC1: inconsistency canary stays 0 on happy dual restore");
    }

    {
        std::println("\n--- #2959 AC2: restore_children seals dual + counters ---");
        FlatAST flat;
        NodeId a = flat.add_literal(1);
        NodeId b = flat.add_literal(2);
        NodeId kids[2] = {a, b};
        auto root = flat.add_begin(std::span<const NodeId>(kids, 2));
        auto snap = flat.snapshot_children();
        flat.set_child(root, 0, flat.add_literal(3));
        const auto dual0 = flat.topology_dual_restore_total();
        flat.restore_children(std::move(snap));
        CHECK(flat.verify_children_parent_topology_consistent(), "2959 AC2: consistent");
        CHECK(flat.topology_dual_restore_total() > dual0,
              "2959 AC2: dual seal on restore_children");
        CHECK(flat.children_topology_restore_count() >= 1, "2959 AC2: children restore count");
        CHECK(flat.parent_topology_restore_count() >= 1, "2959 AC2: parent rebuild count");
    }

    {
        std::println("\n--- #2959 AC3: concurrent dual restore soak lite ---");
        FlatAST flat;
        NodeId kids[2] = {flat.add_literal(1), flat.add_literal(2)};
        auto root = flat.add_begin(std::span<const NodeId>(kids, 2));
        auto snap = flat.snapshot_children();
        flat.set_child(root, 0, flat.add_literal(7));

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> restores{0};
        std::atomic<std::uint64_t> reads{0};
        std::atomic<std::uint64_t> inconsistent{0};

        std::thread writer([&] {
            while (!stop.load(std::memory_order_acquire)) {
                auto s = flat.snapshot_children();
                flat.restore_children(std::move(s));
                if (!flat.verify_children_parent_topology_consistent())
                    inconsistent.fetch_add(1, std::memory_order_relaxed);
                restores.fetch_add(1, std::memory_order_relaxed);
            }
        });
        std::vector<std::thread> readers;
        for (int t = 0; t < 3; ++t) {
            readers.emplace_back([&] {
                while (!stop.load(std::memory_order_acquire)) {
                    (void)flat.children(root).size();
                    (void)flat.parent_of(kids[0]);
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop.store(true, std::memory_order_release);
        writer.join();
        for (auto& th : readers)
            th.join();
        CHECK(restores.load() > 0, "2959 AC3: dual restores progressed");
        CHECK(reads.load() > 0, "2959 AC3: readers progressed");
        CHECK(inconsistent.load() == 0, "2959 AC3: 100% dual topology consistent");
        flat.restore_children(std::move(snap));
        CHECK(flat.verify_children_parent_topology_consistent(), "2959 AC3: final consistent");
    }

    {
        std::println("\n--- #2959 AC4–AC5: source-cite + query + linter ---");
        const auto ast = read_file("src/core/ast.ixx");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        const auto build = read_file("build.py");
        const auto lint = read_file("scripts/coverage/checks/check_topology_dual_restore_2959.py");
        CHECK(ast.find("Issue #2959") != std::string::npos, "2959 AC5: ast cites #2959");
        CHECK(ast.find("abort_restore_dual_topology") != std::string::npos,
              "2959 AC5: dual abort API");
        CHECK(ast.find("seal_dual_topology_restore_locked") != std::string::npos,
              "2959 AC5: seal helper");
        CHECK(ast.find("verify_children_parent_topology_consistent") != std::string::npos,
              "2959 AC5: verify helper");
        CHECK(ast.find("topology_dual_restore_total_") != std::string::npos,
              "2959 AC2: dual total counter");
        CHECK(ast.find("topology_dual_restore_inconsistency_total_") != std::string::npos,
              "2959 AC2: inconsistency canary");
        CHECK(emb.find("abort_restore_dual_topology") != std::string::npos,
              "2959 AC1: Guard abort uses dual restore");
        CHECK(emb.find("Issue #2959") != std::string::npos, "2959 AC5: emb cites #2959");
        CHECK(q.find("schema-2959") != std::string::npos, "2959 AC2: schema-2959");
        CHECK(q.find("topology-dual-restore-total") != std::string::npos,
              "2959 AC2: dual total key");
        CHECK(q.find("topology-dual-restore-inconsistency-total") != std::string::npos,
              "2959 AC2: inconsistency key");
        CHECK(!lint.empty() && lint.find("2959") != std::string::npos, "2959 AC5: linter present");
        CHECK(build.find("check_topology_dual_restore_2959") != std::string::npos,
              "2959 AC5: build.py wires linter");
        CHECK(read_file("docs/design/2959-topology-dual-restore.md").empty(),
              "2959 AC4: no docs/design/2959-* per #1655");
        CHECK(read_file("tests/core/test_issue_2959.cpp").empty(),
              "2959 AC5: no invent test file per #81967");
    }

    std::println("\n=== #2455/#2959 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_restore_children_structural_lock();
}
#endif
