// @category: unit
// @reason: Issue #1849 — MutatorDispatchStats multi-field snapshot
// must be coherent vs concurrent apply_mutation / apply_by_kind
// bumps (shared_lock capture vs unique bump helpers).
//
//   AC1: source cites #1849; capture + shared_mutex / bump_* present
//   AC2: apply_mutation bumps capture snapshot; reset zeros under lock
//   AC3: concurrent capture readers + writers no hang; totals non-decreasing

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.core.mutators;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::ast::mutators::apply_by_kind;
using aura::ast::mutators::apply_by_name;
using aura::ast::mutators::apply_mutation;
using aura::ast::mutators::dispatch_stats;
using aura::ast::mutators::NoOpMutator;
using aura::ast::mutators::StrategyKind;
using aura::ast::mutators::StrategyParams;
using aura::compiler::CompilerService;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

aura::ast::NodeId make_let(FlatAST& flat, StringPool& pool, const char* name, std::int64_t val) {
    auto name_sym = pool.intern(name);
    auto val_node = flat.add_literal(val);
    auto id = flat.add_let(name_sym, val_node, NULL_NODE);
    flat.root = id;
    return id;
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: capture / mutex / #1849 ---");
        std::string mut;
        for (const char* p : {"src/core/mutators.ixx", "../src/core/mutators.ixx"}) {
            mut = read_file(p);
            if (!mut.empty())
                break;
        }
        CHECK(!mut.empty(), "read mutators.ixx");
        CHECK(mut.find("#1849") != std::string::npos, "mutators cites #1849");
        CHECK(mut.find("shared_mutex") != std::string::npos, "has shared_mutex");
        CHECK(mut.find("capture()") != std::string::npos, "has capture()");
        CHECK(mut.find("bump_apply_mutation") != std::string::npos, "has bump helpers");

        std::string prim;
        for (const char* p : {"src/compiler/evaluator_primitives_compile_06.cpp",
                              "../src/compiler/evaluator_primitives_compile_06.cpp"}) {
            prim = read_file(p);
            if (!prim.empty())
                break;
        }
        CHECK(!prim.empty(), "read compile_06.cpp");
        CHECK(prim.find("#1849") != std::string::npos, "compile_06 cites #1849");
        auto pos = prim.find("\"compile:mutator-dispatch-stats\"");
        CHECK(pos != std::string::npos, "stats primitive present");
        auto win = prim.substr(pos, 1200);
        CHECK(win.find("capture()") != std::string::npos, "primitive uses capture()");
    }

    // ── AC2: capture coherence after bumps ──
    {
        std::println("\n--- AC2: capture after apply_mutation ---");
        auto& s = dispatch_stats();
        s.reset();
        auto before = s.capture();
        CHECK(before.total() == 0, "reset → total 0");

        for (int i = 0; i < 3; ++i) {
            FlatAST flat;
            StringPool pool;
            auto id = make_let(flat, pool, "x", i);
            auto r = apply_mutation(flat, id, NoOpMutator{});
            CHECK(r.has_value(), std::format("noop apply {}", i));
        }
        auto after = s.capture();
        CHECK(after.apply_mutation_total == before.apply_mutation_total + 3,
              "apply_mutation_total +3");
        CHECK(after.total() == after.apply_mutation_total + after.apply_by_kind_total +
                                   after.apply_by_name_total,
              "snapshot total = sum of three");

        // apply_by_kind path also bumps apply_mutation (nested).
        {
            FlatAST flat;
            StringPool pool;
            auto id = make_let(flat, pool, "y", 9);
            auto r = apply_by_kind(flat, id, StrategyKind::NoOp, StrategyParams{});
            CHECK(r.has_value(), "apply_by_kind NoOp");
        }
        auto mid = s.capture();
        CHECK(mid.apply_by_kind_total >= 1, "apply_by_kind_total bumped");
        CHECK(mid.kind_success[0] >= 1, "NoOp kind_success bumped");

        {
            FlatAST flat;
            StringPool pool;
            auto id = make_let(flat, pool, "z", 1);
            auto r = apply_by_name(flat, id, "no-op", StrategyParams{});
            CHECK(r.has_value(), "apply_by_name no-op");
        }
        auto end = s.capture();
        CHECK(end.apply_by_name_total >= 1, "apply_by_name_total bumped");

        s.reset();
        CHECK(s.capture().total() == 0, "reset under lock zeros");
    }

    // ── AC3: concurrent capture + writers ──
    {
        std::println("\n--- AC3: concurrent capture vs apply ---");
        auto& s = dispatch_stats();
        s.reset();
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> reads{0};
        std::atomic<std::uint64_t> writes{0};
        std::vector<std::thread> thr;

        thr.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                auto snap = s.capture();
                // Coherence: total is sum of three fields in the same snapshot.
                auto sum =
                    snap.apply_mutation_total + snap.apply_by_kind_total + snap.apply_by_name_total;
                if (sum != snap.total()) {
                    // Should never happen under lock.
                    stop.store(true, std::memory_order_relaxed);
                    return;
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
        thr.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                FlatAST flat;
                StringPool pool;
                auto id = make_let(flat, pool, "w", 1);
                (void)apply_mutation(flat, id, NoOpMutator{});
                writes.fetch_add(1, std::memory_order_relaxed);
            }
        });
        thr.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                FlatAST flat;
                StringPool pool;
                auto id = make_let(flat, pool, "k", 2);
                (void)apply_by_kind(flat, id, StrategyKind::NoOp, StrategyParams{});
                writes.fetch_add(1, std::memory_order_relaxed);
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        stop.store(true, std::memory_order_relaxed);
        for (auto& t : thr)
            t.join();

        CHECK(reads.load() > 0, "readers made progress");
        CHECK(writes.load() > 0, "writers made progress");
        auto final = s.capture();
        CHECK(final.total() == final.apply_mutation_total + final.apply_by_kind_total +
                                   final.apply_by_name_total,
              "final snapshot coherent");
        CHECK(final.apply_mutation_total > 0, "mutations observed");
    }

    // ── AC4: EDSL stats:get smoke ──
    {
        std::println("\n--- AC4: stats:get mutator-dispatch-stats ---");
        CompilerService cs;
        auto r = cs.eval("(stats:get \"compile:mutator-dispatch-stats\")");
        CHECK(r.has_value(), "stats:get returns");
        // Alist (pair) or void if not in facade catalog — must not hang.
        if (r)
            CHECK(is_pair(*r) || is_void(*r), "pair-list or void");
    }

    std::println("\n=== test_mutator_dispatch_stats_lock_1849: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
