// test_per_defuse_batch.cpp — batch driver for per_defuse_index family.
// Consolidates 3 issue tests into 1 batch entry (Phase 4+ migration per
// the 5-domain file directive in tests/{test_fiber,test_ir,test_mutation,
// test_observability,test_persist}.cpp headers):
//
//   Issue #411 fu1 follow-up #1  — per-DefUseIndex caller tracking (10 ACs)
//   Issue #1845                  — Guard + try/catch on per-defuse-index-add (3 ACs)
//   Issue #1846                  — tracker spinlock serialization (AC1+AC2/AC4+AC3)
//
// Per-test ctest -R isolation is lost; use `ninja test_per_defuse_batch`
// for on-demand debug build. EXCLUDE_FROM_ALL per AuraDomainTests.cmake
// legacy batch convention (#809_817 / #819_829 precedents).

#include "test_harness.hpp"
#include "compiler/per_defuse_index.h"

#include <atomic>
#include <chrono>
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

namespace aura_per_defuse_batch {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::per_defuse_index::Caller;
using aura::compiler::per_defuse_index::DefUseIndex;
using aura::compiler::per_defuse_index::PerDefUseIndexTracker;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ── Block 1: Issue #411 fu1 follow-up #1 (10 ACs) ──
// Original: tests/test_per_defuse_index.cpp
// Uses local CHECK/CHECK_EQ macros — converted to test_harness CHECK().
static void run_411_fu1_followup1() {
    std::println("\n=== Issue #411 fu1 follow-up #1: per-DefUseIndex tracking (scope-limited) ===");

    // AC1: fresh tracker is empty
    {
        std::println("\n--- AC1: fresh PerDefUseIndexTracker is empty ---");
        PerDefUseIndexTracker t;
        CHECK(t.total_size() == 0u, "fresh tracker total_size == 0");
        CHECK(t.index_count() == 0u, "fresh tracker index_count == 0");
    }

    // AC2: add_caller + get_callers for one index
    {
        std::println("\n--- AC2: add_caller + get_callers for one DefUseIndex ---");
        PerDefUseIndexTracker t;
        t.add_caller(DefUseIndex{"foo"}, Caller{101});
        auto callers = t.get_callers(DefUseIndex{"foo"});
        CHECK(callers.size() == 1u, "foo has 1 caller");
        if (!callers.empty())
            CHECK(callers[0].node_id == 101u, "caller node_id matches");
    }

    // AC3: per-index isolation
    {
        std::println("\n--- AC3: per-DefUseIndex isolation ---");
        PerDefUseIndexTracker t;
        t.add_caller(DefUseIndex{"foo"}, Caller{201});
        t.add_caller(DefUseIndex{"foo"}, Caller{202});
        t.add_caller(DefUseIndex{"bar"}, Caller{301});

        auto foo_callers = t.get_callers(DefUseIndex{"foo"});
        auto bar_callers = t.get_callers(DefUseIndex{"bar"});

        CHECK(foo_callers.size() == 2u, "foo has exactly 2 callers (per-index isolation)");
        CHECK(bar_callers.size() == 1u, "bar has exactly 1 caller (per-index isolation)");
        if (foo_callers.size() == 2) {
            CHECK(foo_callers[0].node_id == 201u, "foo[0] is caller1 (201)");
            CHECK(foo_callers[1].node_id == 202u, "foo[1] is caller2 (202)");
        }
        if (bar_callers.size() == 1) {
            CHECK(bar_callers[0].node_id == 301u, "bar[0] is caller1 (301)");
        }
    }

    // AC4: get_callers for unregistered
    {
        std::println("\n--- AC4: get_callers for unregistered DefUseIndex returns empty ---");
        PerDefUseIndexTracker t;
        t.add_caller(DefUseIndex{"foo"}, Caller{999});
        auto missing = t.get_callers(DefUseIndex{"missing"});
        CHECK(missing.size() == 0u, "unregistered index returns empty caller list");
    }

    // AC5: size_for_index
    {
        std::println("\n--- AC5: size_for_index reports the correct per-index count ---");
        PerDefUseIndexTracker t;
        t.add_caller(DefUseIndex{"foo"}, Caller{1});
        t.add_caller(DefUseIndex{"foo"}, Caller{2});
        t.add_caller(DefUseIndex{"foo"}, Caller{3});
        t.add_caller(DefUseIndex{"bar"}, Caller{1});
        CHECK(t.size_for_index(DefUseIndex{"foo"}) == 3u, "foo has 3 callers");
        CHECK(t.size_for_index(DefUseIndex{"bar"}) == 1u, "bar has 1 caller");
        CHECK(t.size_for_index(DefUseIndex{"missing"}) == 0u, "missing has 0 callers");
    }

    // AC6: total_size
    {
        std::println("\n--- AC6: total_size sums across all indexes ---");
        PerDefUseIndexTracker t;
        t.add_caller(DefUseIndex{"foo"}, Caller{1});
        t.add_caller(DefUseIndex{"foo"}, Caller{2});
        t.add_caller(DefUseIndex{"bar"}, Caller{1});
        t.add_caller(DefUseIndex{"baz"}, Caller{1});
        t.add_caller(DefUseIndex{"baz"}, Caller{2});
        CHECK(t.total_size() == 5u, "total_size == 5 (sum across 3 indexes)");
    }

    // AC7: index_count
    {
        std::println("\n--- AC7: index_count reports the distinct count ---");
        PerDefUseIndexTracker t;
        t.add_caller(DefUseIndex{"foo"}, Caller{1});
        t.add_caller(DefUseIndex{"foo"}, Caller{2});
        t.add_caller(DefUseIndex{"bar"}, Caller{1});
        CHECK(t.index_count() == 2u, "index_count == 2 (foo + bar, dedup'd)");
    }

    // AC8: clear
    {
        std::println("\n--- AC8: clear() removes all state ---");
        PerDefUseIndexTracker t;
        t.add_caller(DefUseIndex{"foo"}, Caller{1});
        t.add_caller(DefUseIndex{"bar"}, Caller{1});
        CHECK(t.total_size() > 0, "tracker non-empty before clear");
        t.clear();
        CHECK(t.total_size() == 0u, "total_size == 0 after clear");
        CHECK(t.index_count() == 0u, "index_count == 0 after clear");
        auto callers = t.get_callers(DefUseIndex{"foo"});
        CHECK(callers.size() == 0u, "foo's caller list empty after clear");
    }

    // AC9: equality
    {
        std::println("\n--- AC9: DefUseIndex equality (FNV-1a hash on name) ---");
        DefUseIndex a{"foo"};
        DefUseIndex b{"foo"};
        DefUseIndex c{"bar"};
        CHECK(a == b, "DefUseIndex with same name are equal");
        CHECK(!(a == c), "DefUseIndex with different names are unequal");
        std::hash<DefUseIndex> hasher;
        CHECK(hasher(a) == hasher(b), "FNV-1a hash is consistent for equal keys");
        CHECK(hasher(a) != hasher(c), "FNV-1a hash differs for different names");
    }

    // AC10: copyable + movable
    {
        std::println("\n--- AC10: copyable + movable ---");
        PerDefUseIndexTracker t;
        t.add_caller(DefUseIndex{"foo"}, Caller{1});
        PerDefUseIndexTracker t2(t);
        CHECK(t2.total_size() == 1u, "copy ctor preserves state");
        PerDefUseIndexTracker t3(std::move(t2));
        CHECK(t3.total_size() == 1u, "move ctor preserves state");
        PerDefUseIndexTracker t4;
        t4 = t3;
        CHECK(t4.total_size() == 1u, "copy assignment preserves state");
    }
}

// ── Block 2: Issue #1845 (3 ACs) ──
// Original: tests/test_per_defuse_index_add_guard_1845.cpp
static void run_1845_add_guard() {
    std::println("\n=== Issue #1845: Guard + try/catch on per-defuse-index-add ===");

    // AC1: source
    {
        std::println("\n--- AC1: Guard + try/catch on per-defuse-index-add ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_06.cpp");
        CHECK(src.find("#1845") != std::string::npos, "cites #1845");
        auto pos = src.find("\"compile:per-defuse-index-add\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = src.substr(pos, 1800);
        CHECK(win.find("MutationBoundaryGuard") != std::string::npos, "uses Guard");
        CHECK(win.find("guard_ok") != std::string::npos, "guard_ok flag");
        CHECK(win.find("add_caller") != std::string::npos, "calls add_caller");
        CHECK(win.find("catch") != std::string::npos, "catch path");
        auto pre = src.substr(pos > 400 ? pos - 400 : 0, 400);
        CHECK(pre.find("#1839") != std::string::npos ||
                  pre.find("non-owning") != std::string::npos ||
                  win.find("#1839") != std::string::npos,
              "documents service ownership");
    }

    // AC2: runtime
    {
        std::println("\n--- AC2: per-defuse-index-add returns size ---");
        CompilerService cs;
        auto r = cs.eval("(compile:per-defuse-index-add \"foo\" 0)");
        CHECK(r && is_int(*r), "returns int");
        CHECK(as_int(*r) >= 0, "size >= 0");
        auto r2 = cs.eval("(compile:per-defuse-index-add \"foo\" 1)");
        CHECK(r2 && is_int(*r2), "second add returns int");
        CHECK(as_int(*r2) >= as_int(*r), "size non-decreasing");
    }

    // AC3: nested guard
    {
        std::println("\n--- AC3: under outer MutationBoundaryGuard ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard outer(ev, &ok);
            CHECK(outer.is_outermost(), "outer is outermost");
            auto r = cs.eval("(compile:per-defuse-index-add \"bar\" 3)");
            CHECK(r && is_int(*r) && as_int(*r) >= 0, "add under outer Guard ok");
            CHECK(ev.mutation_boundary_depth_slot_value() >= 1, "depth held");
        }
        CHECK(ok, "outer guard_ok");
        CHECK(ev.mutation_boundary_depth_slot_value() == 0, "depth 0 after outer");
    }
}

// ── Block 3: Issue #1846 (AC1 + AC2/AC4 + AC3) ──
// Original: tests/test_per_defuse_index_tracker_lock_1846.cpp
static void run_1846_tracker_lock() {
    std::println("\n=== Issue #1846: tracker spinlock serialization ===");

    // AC1: source
    {
        std::println("\n--- AC1: tracker mutex + #1846 ---");
        std::string h;
        for (const char* p :
             {"src/compiler/per_defuse_index.h", "../src/compiler/per_defuse_index.h"}) {
            h = read_file(p);
            if (!h.empty())
                break;
        }
        CHECK(!h.empty(), "read per_defuse_index.h");
        CHECK(h.find("#1846") != std::string::npos, "cites #1846");
        CHECK(h.find("TrackerSpinLock") != std::string::npos ||
                  h.find("atomic_flag") != std::string::npos,
              "has internal lock");
        CHECK(h.find("TrackerLockGuard") != std::string::npos, "RAII lock guard");
        CHECK(h.find("add_caller") != std::string::npos, "add_caller present");

        std::string prim;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            prim = read_file(p);
            if (!prim.empty())
                break;
        }
        CHECK(!prim.empty(), "read compile_06.cpp");
        auto pos = prim.find("\"compile:per-defuse-index-callers\"");
        CHECK(pos != std::string::npos, "callers prim");
        auto pre = prim.substr(pos > 300 ? pos - 300 : 0, 500);
        CHECK(pre.find("#1846") != std::string::npos, "callers cites #1846");
    }

    // AC2/AC4: round-trip
    {
        std::println("\n--- AC2/AC4: add then callers ---");
        CompilerService cs;
        auto a = cs.eval("(compile:per-defuse-index-add \"idx-a\" 42)");
        CHECK(a && is_int(*a), "add returns int");
        auto c = cs.eval("(compile:per-defuse-index-callers \"idx-a\")");
        CHECK(c && is_hash(*c), "callers returns hash");
        auto v = cs.eval("(hash-ref (compile:per-defuse-index-callers \"idx-a\") \"42\")");
        CHECK(v && is_int(*v), "node 42 present");
    }

    // AC3: concurrent stress
    {
        std::println("\n--- AC3: concurrent add_caller + get_callers ---");
        PerDefUseIndexTracker tr;
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> reads{0};
        std::vector<std::thread> thr;
        thr.emplace_back([&]() {
            std::uint32_t n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                tr.add_caller(DefUseIndex{"stress"}, Caller{n++});
            }
        });
        thr.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                auto v = tr.get_callers(DefUseIndex{"stress"});
                (void)v.size();
                (void)tr.size_for_index(DefUseIndex{"stress"});
                (void)tr.total_size();
                (void)tr.index_count();
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
        thr.emplace_back([&]() {
            std::uint32_t n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                tr.add_caller(DefUseIndex{"other"}, Caller{n++});
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        stop.store(true, std::memory_order_relaxed);
        for (auto& t : thr)
            t.join();
        CHECK(reads.load() > 0, "reader made progress");
        CHECK(tr.size_for_index(DefUseIndex{"stress"}) > 0, "writer made progress");
        auto final_v = tr.get_callers(DefUseIndex{"stress"});
        CHECK(final_v.size() == tr.size_for_index(DefUseIndex{"stress"}),
              "post-join size matches vector");
        CHECK(tr.total_size() == tr.size_for_index(DefUseIndex{"stress"}) +
                                     tr.size_for_index(DefUseIndex{"other"}),
              "total_size = sum of indexes");
    }
}

} // namespace aura_per_defuse_batch

int main() {
    aura_per_defuse_batch::run_411_fu1_followup1();
    aura_per_defuse_batch::run_1845_add_guard();
    aura_per_defuse_batch::run_1846_tracker_lock();
    return RUN_ALL_TESTS();
}
