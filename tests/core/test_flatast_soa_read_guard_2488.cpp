// @category: unit
// @reason: Issue #2488 — FlatAST SoAReadGuard / get_soa_safe vs concurrent
//          add_node (Option B′ shared_mutex upgrade of flatast_mutex_).
//
//   AC1: public SoAReadGuard / SoAWriteGuard / get_soa_safe / try_acquire_*
//   AC2: production query:find uses try_acquire_soa_reader_lock (source-cite)
//   AC3: concurrent add_node + get_soa_safe(id) for id < size_before (TSAN)
//   AC4: single-thread get() hot path unchanged (no forced lock)
//   AC5: this test + CMake + gate

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
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

// ── AC1: API surface ──
static void ac1_public_api() {
    std::println("\n--- #2488 AC1: public SoA guard API ---");
    FlatAST flat;
    const auto id = flat.add_node(NodeTag::LiteralInt);
    {
        auto r = flat.try_acquire_soa_reader_lock();
        CHECK(static_cast<bool>(r), "AC1: SoAReadGuard owns lock");
        CHECK(flat.tag(id) == NodeTag::LiteralInt, "AC1: tag under shared lock");
        const auto v = flat.get(id);
        CHECK(v.tag == NodeTag::LiteralInt, "AC1: get under shared lock");
    }
    {
        auto w = flat.begin_soa_write();
        CHECK(static_cast<bool>(w), "AC1: SoAWriteGuard owns exclusive");
    }
    const auto v = flat.get_soa_safe(id);
    CHECK(v.tag == NodeTag::LiteralInt, "AC1: get_soa_safe");
    CHECK(v.int_value == 0, "AC1: get_soa_safe int");
}

// ── AC2: production path source-cite ──
static void ac2_query_find_wired() {
    std::println("\n--- #2488 AC2: query:find uses SoAReadGuard ---");
    auto src = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
    CHECK(!src.empty(), "AC2: read query_workspace");
    auto pos = src.find("query:find");
    CHECK(pos != std::string::npos, "AC2: query:find found");
    CHECK(src.find("(*q_impls)") != std::string::npos || src.find("q_impls") != std::string::npos,
          "AC2: private q_impls (#2628)");
    auto body = src.substr(pos, 1800);
    CHECK(body.find("try_acquire_soa_reader_lock") != std::string::npos ||
              body.find("SoAReadGuard") != std::string::npos,
          "AC2: SoA reader lock in query:find");
    CHECK(src.find("#2488") != std::string::npos || src.find("Issue #2488") != std::string::npos,
          "AC2: cites #2488");

    auto ast = read_file("src/core/ast.ixx");
    CHECK(ast.find("SoAReadGuard") != std::string::npos, "AC2: SoAReadGuard in ast.ixx");
    CHECK(ast.find("get_soa_safe") != std::string::npos, "AC2: get_soa_safe");
    CHECK(ast.find("try_acquire_soa_reader_lock") != std::string::npos, "AC2: try_acquire");
    CHECK(ast.find("OwnedSharedMutex flatast_mutex_") != std::string::npos ||
              ast.find("flatast_mutex_") != std::string::npos,
          "AC2: flatast_mutex_ present");
    CHECK(ast.find("shared_mutex") != std::string::npos, "AC2: shared_mutex domain");
}

// ── AC3: concurrent add_node + get_soa_safe for stable ids ──
static void ac3_concurrent_add_and_read() {
    std::println("\n--- #2488 AC3: concurrent add_node + get_soa_safe ---");
    FlatAST flat;
    constexpr int kPre = 256;
    for (int i = 0; i < kPre; ++i)
        (void)flat.add_node(NodeTag::LiteralInt);
    const auto size_before = flat.size();
    CHECK(size_before == static_cast<std::size_t>(kPre), "AC3: pre-populated");

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> bad{0};
    std::atomic<int> writers_done{0};

    std::thread writer([&]() {
        for (int i = 0; i < 2000; ++i)
            (void)flat.add_node(NodeTag::Variable);
        writers_done.fetch_add(1, std::memory_order_release);
        stop.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire) ||
                   writers_done.load(std::memory_order_acquire) == 0) {
                for (NodeId id = 0; id < static_cast<NodeId>(size_before); ++id) {
                    // get_soa_safe takes SoA shared lock — TSan clean vs add_node.
                    const auto v = flat.get_soa_safe(id);
                    reads.fetch_add(1, std::memory_order_relaxed);
                    if (v.tag != NodeTag::LiteralInt || v.int_value != 0)
                        bad.fetch_add(1, std::memory_order_relaxed);
                }
                if (writers_done.load(std::memory_order_acquire) != 0 &&
                    stop.load(std::memory_order_acquire))
                    break;
            }
            // One more full pass after writer done.
            for (NodeId id = 0; id < static_cast<NodeId>(size_before); ++id) {
                const auto v = flat.get_soa_safe(id);
                reads.fetch_add(1, std::memory_order_relaxed);
                if (v.tag != NodeTag::LiteralInt)
                    bad.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    writer.join();
    for (auto& th : readers)
        th.join();

    CHECK(flat.size() == size_before + 2000, "AC3: writer added 2000");
    CHECK(reads.load() > 0, "AC3: readers ran");
    CHECK(bad.load() == 0, "AC3: pre-existing nodes intact under concurrent adds");

    // Spanning guard: multi-get under one shared lock.
    {
        auto g = flat.try_acquire_soa_reader_lock();
        CHECK(static_cast<bool>(g), "AC3: spanning guard");
        for (NodeId id = 0; id < 16; ++id) {
            CHECK(flat.get(id).tag == NodeTag::LiteralInt, "AC3: get under spanning guard");
        }
    }
}

// ── AC4: lock-free get remains (source) ──
static void ac4_hotpath_get() {
    std::println("\n--- #2488 AC4: single-thread get hot path ---");
    FlatAST flat;
    const auto id = flat.add_node(NodeTag::Call);
    // Bare get() still works without explicit SoA guard (external serial).
    const auto v = flat.get(id);
    CHECK(v.tag == NodeTag::Call, "AC4: bare get");
    auto src = read_file("src/core/ast.ixx");
    // get() body must not force SoAReadGuard (hot path AC4).
    auto gpos = src.find("NodeView get(NodeId id) const");
    CHECK(gpos != std::string::npos, "AC4: get found");
    auto gbody = src.substr(gpos, 800);
    CHECK(gbody.find("SoAReadGuard") == std::string::npos, "AC4: bare get not forced-locked");
}

// ── AC5: gate ──
static void ac5_gate() {
    std::println("\n--- #2488 AC5: CMake wiring ---");
    auto cm = read_file("CMakeLists.txt");
    CHECK(cm.find("test_flatast_soa_read_guard_2488") != std::string::npos, "AC5: CMake");
}

} // namespace

int run_test_flatast_soa_read_guard_2488() {
    std::println("=== Issue #2488: FlatAST SoAReadGuard / get_soa_safe ===");
    ac1_public_api();
    ac2_query_find_wired();
    ac3_concurrent_add_and_read();
    ac4_hotpath_get();
    ac5_gate();
    std::println("\n=== #2488 summary: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_flatast_soa_read_guard_2488();
}
#endif
