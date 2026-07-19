// @category: unit
// @reason: Issue #1846 — PerDefUseIndexTracker must serialize
// add_caller vs get_callers so concurrent
// compile:per-defuse-index-add / -callers cannot UAF on vector
// reallocation. Header-only spinlock (atomic_flag) avoids
// <shared_mutex> redeclare conflicts under C++20 modules.
//
//   AC1: source cites #1846; tracker has internal lock
//   AC2: primitives cite lock / use get_callers after add
//   AC3: concurrent add + get_callers stress does not crash
//   AC4: round-trip add then callers returns registered node

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
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::per_defuse_index::Caller;
using aura::compiler::per_defuse_index::DefUseIndex;
using aura::compiler::per_defuse_index::PerDefUseIndexTracker;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1: source ──
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
        for (const char* p : {"src/compiler/evaluator_primitives_compile_06.cpp",
                              "../src/compiler/evaluator_primitives_compile_06.cpp"}) {
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

    // ── AC2/AC4: round-trip via EDSL ──
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

    // ── AC3: concurrent stress on tracker alone ──
    // Note: size_for_index vs get_callers across two separate
    // locked calls is not required to match (writer can append
    // between them). Under lock, each get_callers copy is a
    // consistent snapshot; size_for_index is self-consistent.
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
                // Snapshot coherence: returned size is the
                // number of Callers in that vector.
                (void)v.size();
                (void)tr.size_for_index(DefUseIndex{"stress"});
                (void)tr.total_size();
                (void)tr.index_count();
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
        // Second writer on a different index — exercises map
        // rehash under concurrent get_callers on "stress".
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
        // After join, single-threaded: size matches snapshot.
        auto final_v = tr.get_callers(DefUseIndex{"stress"});
        CHECK(final_v.size() == tr.size_for_index(DefUseIndex{"stress"}),
              "post-join size matches vector");
        CHECK(tr.total_size() == tr.size_for_index(DefUseIndex{"stress"}) +
                                     tr.size_for_index(DefUseIndex{"other"}),
              "total_size = sum of indexes");
    }

    std::println("\n=== test_per_defuse_index_tracker_lock_1846: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}
