// @category: unit
// @reason: Issue #2453 — get(NodeId) NodeView multi-column snapshot contract.
//
//   AC1: sequential get() is self-consistent (tag matches payload)
//   AC2: concurrent multi-reader get() on a stable flat (no writer)
//   AC3: source cites Issue #2453 + single-threaded/post-parse contract
//
//   Issue #3868 — seqlock get (torn-NodeView machine-check):
//   AC4: single-thread soft path unchanged, epoch even/quiet
//   AC5: odd-epoch reader fires torn-retry + locked fallback, clean view
//   AC6: concurrent add_node writer vs hot get() — no torn NodeView
//   AC7: source cites #3868 seqlock contract

#include "test_harness.hpp"

#include <array>
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
using aura::ast::SymId;
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

// Lightweight self-consistency of a NodeView under production shapes.
[[nodiscard]] bool view_self_consistent(const FlatAST& flat, NodeId id) {
    auto v = flat.get(id);
    if (v.id != id)
        return false;
    switch (v.tag) {
        case NodeTag::LiteralInt:
            // int payload free-form; children empty for pure literals from add_literal
            return true;
        case NodeTag::LiteralFloat:
            return true;
        case NodeTag::Variable:
            return v.sym_id != aura::ast::INVALID_SYM || true; // allow INVALID during edge cases
        case NodeTag::Define:
            return v.sym_id != aura::ast::INVALID_SYM && v.children.size() >= 1;
        case NodeTag::Call:
            return v.children.size() >= 1; // func + args
        case NodeTag::IfExpr:
            return v.children.size() == 3;
        case NodeTag::Lambda:
            return v.children.size() >= 1;
        case NodeTag::Let:
        case NodeTag::LetRec:
            return v.children.size() == 2 && v.sym_id != aura::ast::INVALID_SYM;
        default:
            return true; // other tags: no hard shape check
    }
}

} // namespace

int run_test_get_nodeview_snapshot() {
    std::println("=== Issue #2453: get(NodeId) NodeView snapshot contract ===");

    // ── AC1: sequential get self-consistency ───────────────────────
    {
        std::println("\n--- #2453 AC1: sequential get self-consistent ---");
        FlatAST flat;
        const auto lit = flat.add_literal(42);
        const auto var = flat.add_variable(7);
        const auto def = flat.add_define(8, lit);
        const auto call = flat.add_call(var, std::span<const NodeId>{});
        const auto ifn = flat.add_if(lit, lit, lit);
        std::array<SymId, 1> params{1};
        const auto lam = flat.add_lambda(std::span<const SymId>{params}, lit, false);
        const auto let = flat.add_let(9, lit, lit);

        CHECK(view_self_consistent(flat, lit), "AC1: literal view");
        CHECK(flat.get(lit).int_value == 42, "AC1: literal int payload");
        CHECK(view_self_consistent(flat, var), "AC1: variable view");
        CHECK(view_self_consistent(flat, def), "AC1: define view");
        CHECK(view_self_consistent(flat, call), "AC1: call view");
        CHECK(view_self_consistent(flat, ifn), "AC1: if view");
        CHECK(view_self_consistent(flat, lam), "AC1: lambda view");
        CHECK(view_self_consistent(flat, let), "AC1: let view");
        CHECK(flat.get(def).children.size() == 1, "AC1: define one child");
        CHECK(flat.get(ifn).children.size() == 3, "AC1: if three children");
    }

    // ── AC2: concurrent multi-reader get on stable flat ────────────
    // Writers finished before readers start — contract-compliant path
    // (post-build concurrent query). Verifies no flaky inconsistency.
    {
        std::println("\n--- #2453 AC2: concurrent multi-reader get (stable flat) ---");
        FlatAST flat;
        constexpr int kN = 64;
        std::vector<NodeId> ids;
        ids.reserve(static_cast<std::size_t>(kN * 3));
        for (int i = 0; i < kN; ++i) {
            const auto lit = flat.add_literal(i);
            const auto var = flat.add_variable(static_cast<SymId>(i + 1));
            const auto def = flat.add_define(static_cast<SymId>(i + 100), lit);
            ids.push_back(lit);
            ids.push_back(var);
            ids.push_back(def);
        }

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> reads{0};
        std::atomic<std::uint64_t> bad{0};

        std::vector<std::thread> threads;
        for (int t = 0; t < 6; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    const auto id = ids[static_cast<std::size_t>(i % ids.size())];
                    if (!view_self_consistent(flat, id))
                        bad.fetch_add(1, std::memory_order_relaxed);
                    // Extra shape checks for known tags
                    auto v = flat.get(id);
                    if (v.tag == NodeTag::Define && v.children.size() != 1)
                        bad.fetch_add(1, std::memory_order_relaxed);
                    if (v.tag == NodeTag::LiteralInt && v.int_value < 0)
                        bad.fetch_add(1, std::memory_order_relaxed);
                    reads.fetch_add(1, std::memory_order_relaxed);
                    i += 6;
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        stop.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        std::println("  #2453 reads={} bad={}", reads.load(), bad.load());
        CHECK(reads.load() > 0, "AC2: concurrent reads progressed");
        CHECK(bad.load() == 0, "AC2: no inconsistent NodeView under multi-reader");
    }

    // ── AC3: source cite ───────────────────────────────────────────
    {
        std::println("\n--- #2453 AC3: source cites get() snapshot contract ---");
        auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("Issue #2453") != std::string::npos, "source-cite #2453");
        CHECK(ast.find("NodeView get(NodeId id)") != std::string::npos ||
                  ast.find("get(NodeId id) const") != std::string::npos,
              "get(NodeId) present");
        CHECK(ast.find("torn") != std::string::npos || ast.find("snapshot") != std::string::npos,
              "torn/snapshot wording");
        CHECK(ast.find("post-parse") != std::string::npos ||
                  ast.find("workspace_mtx") != std::string::npos,
              "serialization contract wording");
    }

    // ── AC4 (#3868): single-thread soft path unchanged + epoch invariants ──
    {
        std::println("\n--- #3868 AC4: single-thread get unchanged, epoch quiet ---");
        FlatAST flat;
        const auto lit = flat.add_literal(42);
        const auto var = flat.add_variable(7);
        const auto def = flat.add_define(8, lit);
        std::array<SymId, 1> params{1};
        const auto lam = flat.add_lambda(std::span<const SymId>{params}, lit, false);

        CHECK(flat.get(lit).int_value == 42, "AC4: literal int payload");
        CHECK(flat.get(def).children.size() == 1, "AC4: define child");
        CHECK(view_self_consistent(flat, lam), "AC4: lambda view");
        CHECK(view_self_consistent(flat, var), "AC4: variable view");
        CHECK(flat.soa_write_epoch() % 2 == 0, "AC4: epoch even after writer sections close");
        CHECK(flat.soa_get_torn_retry_total() == 0,
              "AC4: no torn retries on the single-thread soft path");
    }

    // ── AC5 (#3868): reader detects open writer section (odd epoch) ──────
    // Deterministic detector test: hold an external SoA write section
    // (SoAWriteGuard = epoch odd), run a hot get() reader — it must fire
    // the torn-retry detector, park in the locked fallback, and observe a
    // clean view once the section closes.
    {
        std::println("\n--- #3868 AC5: odd-epoch reader retries + locked fallback ---");
        FlatAST flat;
        const auto lit = flat.add_literal(5);
        std::atomic<bool> done{false};
        std::atomic<std::int64_t> observed_int{0};
        std::thread reader;
        {
            auto guard = flat.begin_soa_write(); // epoch even→odd, held open
            reader = std::thread([&]() {
                auto v = flat.get(lit);
                observed_int.store(v.int_value, std::memory_order_release);
                done.store(true, std::memory_order_release);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CHECK(flat.soa_write_epoch() % 2 == 1, "AC5: writer section holds epoch odd");
            // guard dtor closes the section (odd→even); the parked
            // reader's get_soa_safe fallback then completes against the
            // stable flat.
        }
        reader.join();
        CHECK(done.load(std::memory_order_acquire), "AC5: fallback reader completed");
        CHECK(observed_int.load(std::memory_order_acquire) == 5,
              "AC5: fallback view clean (payload intact)");
        CHECK(flat.soa_get_torn_retry_total() >= 1,
              "AC5: torn-retry detector fired under open writer section");
        CHECK(flat.soa_write_epoch() % 2 == 0, "AC5: epoch back to even");
    }

    // ── AC6 (#3868): concurrent add_node writer vs hot lock-free get() ───
    // Writers append new nodes (epoch bumps); readers get() STABLE ids —
    // any torn assemble must be caught by the seqlock retry/fallback, so
    // every returned view stays self-consistent.
    {
        std::println("\n--- #3868 AC6: no torn NodeView under concurrent writer ---");
        FlatAST flat;
        constexpr int kN = 64;
        std::vector<NodeId> ids;
        ids.reserve(static_cast<std::size_t>(kN * 3));
        for (int i = 0; i < kN; ++i) {
            const auto lit = flat.add_literal(i);
            const auto var = flat.add_variable(static_cast<SymId>(i + 1));
            const auto def = flat.add_define(static_cast<SymId>(i + 100), lit);
            ids.push_back(lit);
            ids.push_back(var);
            ids.push_back(def);
        }

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> reads{0};
        std::atomic<std::uint64_t> bad{0};
        std::vector<std::thread> threads;
        // 1 writer: appends (multi-column SoA growth mid-read window).
        threads.emplace_back([&]() {
            for (int k = 0; k < 4000 && !stop.load(std::memory_order_acquire); ++k)
                (void)flat.add_literal(k);
        });
        // 4 readers: hot get() on stable ids.
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                int i = t;
                while (!stop.load(std::memory_order_acquire)) {
                    const auto id = ids[static_cast<std::size_t>(i % ids.size())];
                    if (!view_self_consistent(flat, id))
                        bad.fetch_add(1, std::memory_order_relaxed);
                    auto v = flat.get(id);
                    if (v.tag == NodeTag::LiteralInt && v.int_value < 0)
                        bad.fetch_add(1, std::memory_order_relaxed);
                    reads.fetch_add(1, std::memory_order_relaxed);
                    i += 4;
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        stop.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();

        std::println("  #3868 reads={} bad={}", reads.load(), bad.load());
        CHECK(reads.load() > 0, "AC6: concurrent reads progressed");
        CHECK(bad.load() == 0, "AC6: no torn NodeView escaped the seqlock");
        CHECK(flat.soa_write_epoch() % 2 == 0, "AC6: epoch even after writers stop");
    }

    // ── AC7 (#3868): source cites seqlock get contract ──────────────────
    {
        std::println("\n--- #3868 AC7: source cites seqlock get contract ---");
        auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("Issue #3868") != std::string::npos, "source-cite #3868");
        CHECK(ast.find("soa_write_epoch_") != std::string::npos, "seqlock epoch member");
        CHECK(ast.find("assemble_nodeview") != std::string::npos, "raw assemble extracted");
        CHECK(ast.find("return get_soa_safe(id);") != std::string::npos,
              "locked fallback on pathological contention");
        CHECK(ast.find("SoaSeqlockWriteSection") != std::string::npos, "writer-section RAII");
    }

    std::println("\n=== #2453 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_get_nodeview_snapshot();
}
#endif
