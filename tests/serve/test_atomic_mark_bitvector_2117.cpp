// @category: unit
// @reason: Issue #2117 — atomic MarkBitVector for concurrent multi-fiber mark.
//
//   AC1: multi-thread concurrent set same/adjacent bits → all set (no lost update)
//   AC2: mark_from_roots / test / count_dead still correct (GC suite path)
//   AC3: atomic storage + relaxed order documented (query + source)
//   AC4: concurrent mark stress + source wiring

#include "test_harness.hpp"
#include "serve/gc_coordinator.h"

#include <atomic>
#include <chrono>
#include <cstdint>
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
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::serve::GCCollector;
using aura::serve::GCRootSet;
using aura::serve::MarkBitVector;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:gc-mark-size-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
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

static void ac1_concurrent_set_no_lost_update() {
    std::println("\n--- AC1: concurrent set same/adjacent bits — no lost update ---");
    CHECK(MarkBitVector::is_atomic_storage(), "atomic storage");

    constexpr size_t kBits = 4096;
    MarkBitVector bits(kBits);
    CHECK(bits.size() == kBits, "size");

    constexpr int kThreads = 8;
    constexpr int kIters = 2000;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&bits, t]() {
            // Each thread hammers a shared region + its own region.
            for (int i = 0; i < kIters; ++i) {
                // Same bits from all threads (contention hot path)
                bits.set(static_cast<size_t>(i % 64));
                bits.set(static_cast<size_t>(63 - (i % 64)));
                // Adjacent bits across word boundaries
                bits.set(static_cast<size_t>(62 + (i % 4))); // 62..65 spans word
                // Thread-private stripe
                bits.set(static_cast<size_t>(256 + t * 128 + (i % 128)));
            }
        });
    }
    for (auto& w : workers)
        w.join();

    // Shared contention region must be fully set
    for (size_t i = 0; i < 64; ++i)
        CHECK(bits.test(i), "shared bit set");
    for (size_t i = 62; i <= 65; ++i)
        CHECK(bits.test(i), "word-boundary adjacent set");
    // Private stripes
    for (int t = 0; t < kThreads; ++t) {
        for (size_t j = 0; j < 128; ++j)
            CHECK(bits.test(static_cast<size_t>(256 + t * 128 + j)), "private stripe set");
    }
    // Untouched high region stays clear
    CHECK(!bits.test(kBits - 1), "untouched high bit clear");
    CHECK(bits.count_dead() < kBits, "some live bits");
}

static void ac2_mark_from_roots_api() {
    std::println("\n--- AC2: mark_from_roots / count_dead still correct ---");
    GCCollector gc(nullptr);
    GCRootSet roots;
    roots.string_roots = {0, 2, 100};
    roots.pair_roots = {1, 50};
    roots.closure_roots = {3};
    constexpr size_t kS = 200, kP = 100, kC = 50;
    gc.mark_from_roots(roots, kS, kP, kC);
    CHECK(gc.string_marks_size() == kS, "string size");
    CHECK(gc.pair_marks_size() == kP, "pair size");
    CHECK(gc.closure_marks_size() == kC, "closure size");
    CHECK(gc.string_mark(0) && gc.string_mark(2) && gc.string_mark(100), "string roots");
    CHECK(gc.pair_mark(1) && gc.pair_mark(50), "pair roots");
    CHECK(gc.closure_mark(3), "closure root");
    CHECK(!gc.string_mark(1), "unmarked string");
    CHECK(gc.string_marks_dead_count() == kS - 3, "dead = size - 3 live");
}

static void ac3_query_and_docs() {
    std::println("\n--- AC3: query schema-2117 + STW relaxed docs ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:gc-mark-size-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema-2117") == 2117, "schema-2117");
    CHECK(href(cs, "issue-2117") == 2117, "issue-2117");
    CHECK(href(cs, "mark-bitvector-atomic-wired") == 1, "wired");
    CHECK(href(cs, "mark-bitvector-atomic-storage") == 1, "atomic storage");
    CHECK(href(cs, "mark-bitvector-relaxed-order") == 1, "relaxed order");
    // AC3: STW path uses relaxed atomics — fence is safepoint (documented).
    auto hdr = read_file("src/serve/gc_coordinator.h");
    CHECK(hdr.find("memory_order_relaxed") != std::string::npos, "relaxed in API");
    CHECK(hdr.find("safepoint") != std::string::npos || hdr.find("STW") != std::string::npos,
          "STW/safepoint doc");
}

static void ac4_concurrent_mark_stress_and_source() {
    std::println("\n--- AC4: concurrent mark stress + source wiring ---");
    // Multi-thread mark_from_roots-style concurrent sets on one vector.
    constexpr size_t kN = 10000;
    MarkBitVector bits(kN);
    constexpr int kThreads = 4;
    std::atomic<int> done{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&bits, t, &done]() {
            for (size_t i = t; i < kN; i += static_cast<size_t>(kThreads))
                bits.set(i);
            done.fetch_add(1);
        });
    }
    for (auto& w : workers)
        w.join();
    CHECK(done.load() == kThreads, "all threads done");
    for (size_t i = 0; i < kN; ++i)
        CHECK(bits.test(i), "all bits set under partition mark");
    CHECK(bits.count_dead() == 0, "zero dead after full mark");

    auto hdr = read_file("src/serve/gc_coordinator.h");
    auto cpp = read_file("src/serve/gc_coordinator.cpp");
    CHECK(hdr.find("Issue #2117") != std::string::npos || hdr.find("#2117") != std::string::npos,
          "header cites #2117");
    CHECK(hdr.find("std::atomic<std::uint64_t>") != std::string::npos, "atomic words");
    CHECK(hdr.find("fetch_or") != std::string::npos, "fetch_or set");
    // Storage is atomic words (comment may mention legacy vector<bool>).
    CHECK(hdr.find("not vector<bool>") != std::string::npos ||
              hdr.find("std::vector<bool>") == std::string::npos,
          "not vector<bool> storage");
    CHECK(cpp.find(".reset()") != std::string::npos, "sweep uses reset");
}

} // namespace

int run_test_atomic_mark_bitvector_2117() {
    std::println("=== Issue #2117: atomic MarkBitVector concurrent mark ===");
    ac1_concurrent_set_no_lost_update();
    ac2_mark_from_roots_api();
    ac3_query_and_docs();
    ac4_concurrent_mark_stress_and_source();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_atomic_mark_bitvector_2117();
}
#endif
