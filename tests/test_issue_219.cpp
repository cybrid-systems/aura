// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_219.cpp — Issue #219: gap-buffer child_data_
// for O(1) insert/remove in FlatAST.
//
// This test verifies the GapBuffer template defined in
// src/core/gap_buffer.hh. The test struct `GapBufferLike` is a
// hand-written copy of the production GapBuffer (same layout,
// public for testability) so this test doesn't need to import
// the production module (which has the GCC 16.1 std module
// + P2996 reflection conflict).
//
// Test scenarios:
//   1. basic push_back / size / operator[] / clear
//   2. insert at arbitrary positions (front, middle, end)
//   3. erase at arbitrary positions
//   4. 5000-element insert: 100 ops, each < 100µs (perf smoke)
//   5. 5000-element remove: 100 ops, each < 100µs (perf smoke)
//   6. 1000 inserts + 1000 erases + 1 compact: < 10ms (perf smoke)
//   7. roundtrip: gap buffer survives clear() + reconstruct
//   8. reserve + grow cycle (capacity doubling)
//   9. large insert/erase on pre-built AST-like data
//  10. flat wire format v1 roundtrip with GapBuffer-like
//      (verifies the columnar layout treats GapBuffer as a
//       range via operator[])


#include "../src/core/gap_buffer.hh"

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;



#define PRINTLN(msg) std::fprintf(stdout, "%s\n", (msg))

using GB = aura::ast::GapBuffer<std::uint32_t>;

// ── Test 1: basic push_back / size / operator[] / clear ──────
void test_1_basic() {
    PRINTLN("\n--- Test 1: basic push_back / size / operator[] / clear ---");
    GB gb;
    CHECK(gb.empty(), "fresh gb is empty");
    CHECK(gb.size() == 0, "fresh gb size == 0");
    CHECK(gb.capacity() == 0, "fresh gb capacity == 0");

    gb.push_back(10);
    gb.push_back(20);
    gb.push_back(30);
    CHECK(gb.size() == 3, "size 3 after 3 push_backs");
    CHECK(!gb.empty(), "not empty after pushes");
    CHECK(gb[0] == 10, "[0] == 10");
    CHECK(gb[1] == 20, "[1] == 20");
    CHECK(gb[2] == 30, "[2] == 30");
    CHECK(gb.front() == 10, "front == 10");
    CHECK(gb.back() == 30, "back == 30");

    gb.clear();
    CHECK(gb.empty(), "empty after clear");
    CHECK(gb.size() == 0, "size 0 after clear");
    // capacity should be preserved across clear (no realloc)
    CHECK(gb.capacity() >= 3, "capacity preserved across clear");
}

// ── Test 2: insert at arbitrary positions ─────────────────────
void test_2_insert_positions() {
    PRINTLN("\n--- Test 2: insert at arbitrary positions ---");
    GB gb;
    gb.push_back(1);
    gb.push_back(3);
    gb.push_back(5);
    // gb: [1, 3, 5]

    // Insert in the middle.
    gb.insert(1, 2);
    CHECK(gb.size() == 4, "size 4 after middle insert");
    CHECK(gb[0] == 1 && gb[1] == 2 && gb[2] == 3 && gb[3] == 5, "[1,2,3,5] after middle insert");

    // Insert at the front.
    gb.insert(0, 0);
    CHECK(gb.size() == 5, "size 5 after front insert");
    CHECK(gb[0] == 0 && gb[1] == 1 && gb[2] == 2 && gb[3] == 3 && gb[4] == 5, "[0,1,2,3,5] after front insert");

    // Insert at the end (push_back equivalent).
    gb.insert(gb.size(), 6);
    CHECK(gb.size() == 6, "size 6 after end insert");
    CHECK(gb[5] == 6, "last == 6 after end insert");

    // Multiple inserts at the same position (extending).
    gb.insert(3, 99);
    gb.insert(3, 88);
    CHECK(gb.size() == 8, "size 8 after two inserts at pos 3");
    CHECK(gb[3] == 88, "[3] == 88 (second insert first in log)");
    CHECK(gb[4] == 99, "[4] == 99 (first insert second in log)");
    CHECK(gb[5] == 3, "[5] == 3 (original [3] shifted)");
}

// ── Test 3: erase at arbitrary positions ──────────────────────
void test_3_erase_positions() {
    PRINTLN("\n--- Test 3: erase at arbitrary positions ---");
    GB gb;
    for (std::uint32_t i = 0; i < 5; ++i) gb.push_back(i * 10);
    // gb: [0, 10, 20, 30, 40]

    gb.erase(2);  // remove the '20'
    CHECK(gb.size() == 4, "size 4 after middle erase");
    CHECK(gb[0] == 0 && gb[1] == 10 && gb[2] == 30 && gb[3] == 40, "[0,10,30,40] after middle erase");

    gb.erase(0);  // remove from front
    CHECK(gb.size() == 3, "size 3 after front erase");
    CHECK(gb[0] == 10 && gb[1] == 30 && gb[2] == 40, "[10,30,40] after front erase");

    gb.erase(gb.size() - 1);  // remove from end
    CHECK(gb.size() == 2, "size 2 after end erase");
    CHECK(gb[0] == 10 && gb[1] == 30, "[10,30] after end erase");

    gb.erase(0);
    gb.erase(0);
    CHECK(gb.empty(), "empty after erasing all");

    // Erase on empty should be a no-op.
    gb.erase(0);
    CHECK(gb.empty(), "erase on empty is no-op");
}

// ── Test 4: 5000-element insert: 100 ops, each < 10µs ─────────
void test_4_perf_insert() {
    PRINTLN("\n--- Test 4: 5000-element insert: 100 ops, each < 100µs ---");
    // Pre-build a 5000-element buffer (sequential, gap at end).
    GB gb;
    for (std::uint32_t i = 0; i < 5000; ++i) gb.push_back(i);
    CHECK(gb.size() == 5000, "pre-built size 5000");

    // 100 inserts at random positions.
    std::mt19937 rng(12345);
    std::uniform_int_distribution<std::uint32_t> pos_dist(0, static_cast<std::uint32_t>(gb.size()));

    // Warm-up: do a few inserts first to move the gap away from the end.
    for (int i = 0; i < 5; ++i) {
        gb.insert(pos_dist(rng), 99999);
    }

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        gb.insert(pos_dist(rng), 99999);
    }
    auto end = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    auto per_op_us = static_cast<double>(us) / 100.0;
    std::fprintf(stdout, "  INFO: 100 inserts took %ld µs (%.2f µs/op)\n",
                 static_cast<long>(us), per_op_us);
    CHECK(per_op_us < 100.0, "per-op < 100µs (perf smoke, #219)");
}

// ── Test 5: 5000-element remove: 100 ops, each < 100µs ───────
void test_5_perf_erase() {
    PRINTLN("\n--- Test 5: 5000-element remove: 100 ops, each < 100µs ---");
    GB gb;
    for (std::uint32_t i = 0; i < 5000; ++i) gb.push_back(i);
    CHECK(gb.size() == 5000, "pre-built size 5000");

    std::mt19937 rng(54321);
    std::uniform_int_distribution<std::uint32_t> pos_dist(0, static_cast<std::uint32_t>(gb.size()) - 1);

    // Warm-up: do a few erases first.
    for (int i = 0; i < 5; ++i) {
        gb.erase(pos_dist(rng));
    }

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        gb.erase(pos_dist(rng));
    }
    auto end = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    auto per_op_us = static_cast<double>(us) / 100.0;
    std::fprintf(stdout, "  INFO: 100 erases took %ld µs (%.2f µs/op)\n",
                 static_cast<long>(us), per_op_us);
    CHECK(per_op_us < 100.0, "per-op < 100µs (perf smoke, #219)");
}

// ── Test 6: 1000 inserts + 1000 erases + 1 compact: < 10ms ────
void test_6_perf_mixed() {
    PRINTLN("\n--- Test 6: 1000 inserts + 1000 erases + 1 compact: < 10ms ---");
    GB gb;
    for (int i = 0; i < 100; ++i) gb.push_back(i);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        gb.insert(static_cast<std::size_t>(i) % 100, static_cast<std::uint32_t>(i));
    }
    for (int i = 0; i < 1000; ++i) {
        gb.erase(static_cast<std::size_t>(i) % gb.size());
    }
    gb.compact();
    auto end = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    std::fprintf(stdout, "  INFO: 1000+1000+compact took %ld µs\n", static_cast<long>(us));
    CHECK(us < 10'000, "1000+1000+compact < 10ms (perf smoke, #219)");
}

// ── Test 7: roundtrip: clear + reconstruct preserves state ───
void test_7_clear_reconstruct() {
    PRINTLN("\n--- Test 7: clear + reconstruct ---");
    GB gb;
    for (std::uint32_t i = 0; i < 100; ++i) gb.push_back(i * 7);
    CHECK(gb.size() == 100, "pre-clear size 100");

    gb.clear();
    CHECK(gb.empty(), "empty after clear");

    // Reconstruct.
    for (std::uint32_t i = 0; i < 50; ++i) gb.push_back(i * 11);
    CHECK(gb.size() == 50, "size 50 after reconstruct");
    for (std::uint32_t i = 0; i < 50; ++i) {
        CHECK(gb[i] == i * 11, "reconstructed element matches");
    }
}

// ── Test 8: reserve + grow cycle (capacity doubling) ────────
void test_8_reserve_grow() {
    PRINTLN("\n--- Test 8: reserve + grow cycle (capacity doubling) ---");
    GB gb;
    gb.reserve(100);
    CHECK(gb.capacity() >= 100, "capacity >= 100 after reserve(100)");
    CHECK(gb.size() == 0, "size still 0 after reserve");

    for (std::uint32_t i = 0; i < 200; ++i) gb.push_back(i);
    CHECK(gb.size() == 200, "size 200 after pushes");
    // Capacity should have grown (initial 100, then doubled).
    CHECK(gb.capacity() >= 200, "capacity grew to >= 200");

    for (std::uint32_t i = 0; i < 200; ++i) {
        CHECK(gb[i] == i, "element matches after grow");
    }

    // shrink_to_fit should bring capacity down to size.
    gb.shrink_to_fit();
    CHECK(gb.capacity() == gb.size(), "capacity == size after shrink_to_fit");
}

// ── Test 9: large insert/erase on pre-built AST-like data ────
void test_9_ast_pattern() {
    PRINTLN("\n--- Test 9: large insert/erase on pre-built AST-like data ---");
    // Simulate FlatAST child_data_: pre-build 5000 nodes, each
    // with ~3 children, then do random insert_child / remove_child.
    GB gb;
    constexpr std::uint32_t num_nodes = 5000;
    constexpr std::uint32_t avg_children = 3;
    for (std::uint32_t i = 0; i < num_nodes * avg_children; ++i) {
        gb.push_back(i);
    }
    CHECK(gb.size() == num_nodes * avg_children, "pre-built AST-like size");

    // Random inserts and removes.
    std::mt19937 rng(99999);
    std::uniform_int_distribution<std::uint32_t> pos_dist(0, static_cast<std::uint32_t>(gb.size()) - 1);
    std::uniform_int_distribution<int> op_dist(0, 1);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        if (op_dist(rng) == 0) {
            // insert
            gb.insert(pos_dist(rng), 99999);
        } else {
            // erase
            gb.erase(pos_dist(rng));
        }
    }
    auto end = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    auto per_op_us = static_cast<double>(us) / 100.0;
    std::fprintf(stdout, "  INFO: 100 mixed ops took %ld µs (%.2f µs/op)\n",
                 static_cast<long>(us), per_op_us);
    CHECK(per_op_us < 100.0, "per-op < 100µs (mixed insert/erase)");
}

// ── Test 10: flat wire format v1 roundtrip with GapBuffer ────
void test_10_wire_format() {
    PRINTLN("\n--- Test 10: flat wire format v1 roundtrip ---");
    // Simulate the FlatAST serialize_soa format for child_data_:
    // [u32 count] + count * 4 bytes raw.
    GB gb;
    constexpr std::uint32_t num_children = 1000;
    for (std::uint32_t i = 0; i < num_children; ++i) gb.push_back(i * 13);

    // Serialize (using the columnar format the production uses).
    std::vector<char> buf;
    std::uint32_t count = static_cast<std::uint32_t>(gb.size());
    buf.insert(buf.end(), reinterpret_cast<char*>(&count),
               reinterpret_cast<char*>(&count) + 4);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t v = gb[i];
        buf.insert(buf.end(), reinterpret_cast<char*>(&v),
                   reinterpret_cast<char*>(&v) + 4);
    }
    CHECK(buf.size() == 4 + num_children * 4, "serialized buf size matches");

    // Deserialize back into a new GapBuffer.
    GB gb2;
    std::size_t pos = 0;
    std::memcpy(&count, &buf[pos], 4); pos += 4;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t v;
        std::memcpy(&v, &buf[pos], 4); pos += 4;
        gb2.push_back(v);
    }
    CHECK(gb2.size() == num_children, "deserialized size matches");
    for (std::uint32_t i = 0; i < num_children; ++i) {
        CHECK(gb2[i] == i * 13, "deserialized element matches");
    }
    CHECK(pos == buf.size(), "all bytes consumed");
}

int main() {
    test_1_basic();
    test_2_insert_positions();
    test_3_erase_positions();
    test_4_perf_insert();
    test_5_perf_erase();
    test_6_perf_mixed();
    test_7_clear_reconstruct();
    test_8_reserve_grow();
    test_9_ast_pattern();
    test_10_wire_format();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
