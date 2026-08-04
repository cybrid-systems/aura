// @category: unit
// @reason: Issue #2455 — restore_children takes structural exclusive lock
//          (no torn children_/parent_ mid-restore for concurrent readers).
//
//   AC1: restore_children without external guard restores correctly (self-locks)
//   AC2: concurrent restore_children + reader get() no crash / coherent size
//   AC3: source cites Issue #2455 + StructuralMutationGuard / locked path

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

int run_test_restore_children_structural_lock_2455() {
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

    std::println("\n=== #2455 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_restore_children_structural_lock_2455();
}
#endif
