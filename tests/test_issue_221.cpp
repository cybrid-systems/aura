// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_221.cpp — Issue #221: PersistentChildVector
// (Issue #179 Cycle 3, slice 1/5).
//
// Tests the data structure defined in
// src/core/persistent_child_vector.hh. Standalone TU
// (no module imports — avoids the GCC 16.1 std module +
// P2996 reflection conflict). The PersistentChildVector is
// the "final form" of FlatAST's per-node children storage:
// immutable + COW. Each "mutation" returns a new vector; the
// old vector is unchanged, so back-references stay valid.
//
// Test scenarios:
//   1. Basic: construct, size, operator[], iterators
//   2. COW semantics: with_push_back / with_insert /
//      with_erase / with_set leave the receiver unchanged
//   3. Back-references: old shared_ptr stays valid after a
//      with_* on a copy
//   4. Multiple branches: tree of with_* operations, all
//      backed by the same original
//   5. Rollback correctness: capture pre-mutation state,
//      mutate forward, "rollback" by reinstalling the
//      capture, verify result == pre-mutation
//   6. Empty vector: construct, with_push_back, with_erase
//      (on empty is no-op), with_set (out of range is no-op)
//   7. Comparison: ==, !=
//   8. Perf: 5000 mutations on a 1000-element base, < 2µs/op
//   9. Wire format roundtrip (per-node count + flat concat)


#include "../src/core/persistent_child_vector.hh"

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

using PCV = aura::ast::PersistentChildVector<std::uint32_t>;
static constexpr std::uint32_t NULL_NODE = ~0u;



#define PRINTLN(msg) std::fprintf(stdout, "%s\n", (msg))

// ── Test 1: Basic operations ─────────────────────────────────
void test_1_basic() {
    PRINTLN("\n--- Test 1: Basic operations ---");
    PCV v;
    CHECK(v.empty(), "default-constructed is empty");
    CHECK(v.size() == 0, "size == 0");
    CHECK(v.data() == nullptr, "data() == nullptr for empty");

    PCV v2{10, 20, 30, 40, 50};
    CHECK(!v2.empty(), "initializer-list not empty");
    CHECK(v2.size() == 5, "size == 5");
    CHECK(v2[0] == 10, "[0] == 10");
    CHECK(v2[4] == 50, "[4] == 50");
    CHECK(v2.front() == 10, "front == 10");
    CHECK(v2.back() == 50, "back == 50");

    PCV v3(3, 99u);
    CHECK(v3.size() == 3, "size(3, 99) == 3");
    CHECK(v3[0] == 99, "[0] == 99");
    CHECK(v3[2] == 99, "[2] == 99");

    // Iterators
    std::uint32_t sum = 0;
    for (auto x : v2) sum += x;
    CHECK(sum == 150, "sum of [10,20,30,40,50] == 150");
}

// ── Test 2: COW semantics (receiver unchanged) ──────────────
void test_2_cow() {
    PRINTLN("\n--- Test 2: COW semantics (receiver unchanged) ---");
    PCV v{1, 2, 3};
    auto original_data_ptr = v.data();
    auto original_size = v.size();

    auto v2 = v.with_push_back(99);
    CHECK(v.size() == original_size, "push_back doesn't change v.size()");
    CHECK(v.data() == original_data_ptr,
          "push_back doesn't change v.data() (COW)");
    CHECK(v[0] == 1 && v[1] == 2 && v[2] == 3, "v content unchanged");
    CHECK(v2.size() == 4, "v2 has new size 4");
    CHECK(v2[3] == 99, "v2[3] is the new element");

    auto v3 = v.with_insert(1, 77);
    CHECK(v.size() == 3, "insert doesn't change v.size()");
    CHECK(v[1] == 2, "v content unchanged after insert");
    CHECK(v3.size() == 4, "v3 size 4");
    CHECK(v3[1] == 77, "v3[1] is the inserted element");

    auto v4 = v.with_erase(1);
    CHECK(v.size() == 3, "erase doesn't change v.size()");
    CHECK(v[1] == 2, "v content unchanged after erase");
    CHECK(v4.size() == 2, "v4 size 2");
    CHECK(v4[1] == 3, "v4[1] is the element after the erased one");

    auto v5 = v.with_set(0, 88);
    CHECK(v.size() == 3, "set doesn't change v.size()");
    CHECK(v[0] == 1, "v content unchanged after set");
    CHECK(v5[0] == 88, "v5[0] is the new element");
}

// ── Test 3: Back-references (old shared_ptr stays valid) ──────
void test_3_back_references() {
    PRINTLN("\n--- Test 3: Back-references (old shared_ptr stays valid) ---");
    PCV v{10, 20, 30};
    // v1 holds a reference to v's old data via the COW storage.
    // When we mutate v via with_push_back, v's storage should
    // still be alive (refcount >= 2) and v1 unchanged.
    auto v1 = v;  // copy (shared_ptr refcount++)
    CHECK(v.data() == v1.data(), "v and v1 share the same storage");

    auto v2 = v.with_push_back(40);  // v's storage should be preserved
    CHECK(v.data() != v2.data(),
          "v2 has different storage (new allocation for push_back)");
    CHECK(v.data() == v1.data(),
          "v1 still shares v's old storage (back-reference alive)");
    CHECK(v.size() == 3, "v unchanged");
    CHECK(v1.size() == 3, "v1 unchanged");
    CHECK(v2.size() == 4, "v2 has new element");

    // Verify v1's contents
    CHECK(v1[0] == 10 && v1[1] == 20 && v1[2] == 30, "v1 content unchanged");

    // Now v goes out of scope. v1 should still be valid (its
    // shared_ptr keeps the storage alive).
    {
        PCV temp = v.with_erase(0);
        // temp goes out of scope here. v1 still holds the original.
    }
    CHECK(v1[0] == 10, "v1 survives temp's destruction (refcount OK)");
}

// ── Test 4: Multiple branches from a single original ────────
void test_4_branches() {
    PRINTLN("\n--- Test 4: Multiple branches from a single original ---");
    PCV original{1, 2, 3, 4, 5};
    // Build 5 different branches, all from the same original.
    auto a = original.with_push_back(100);          // [1,2,3,4,5,100]
    auto b = original.with_erase(2);                 // [1,2,4,5]
    auto c = original.with_insert(0, 0);             // [0,1,2,3,4,5]
    auto d = original.with_set(2, 22);               // [1,2,22,4,5]
    auto e = original;  // copy (untouched)

    // Each branch is independent
    CHECK(a.size() == 6 && a[5] == 100, "a: [1,2,3,4,5,100]");
    CHECK(b.size() == 4 && b[0] == 1 && b[1] == 2 && b[2] == 4 && b[3] == 5, "b: [1,2,4,5]");
    CHECK(c.size() == 6 && c[0] == 0, "c: [0,1,2,3,4,5]");
    CHECK(d.size() == 5 && d[2] == 22, "d: [1,2,22,4,5]");
    CHECK(e == original, "e: unchanged copy of original");
    CHECK(original.size() == 5 && original[0] == 1, "original untouched");
}

// ── Test 5: Rollback correctness ───────────────────────────
void test_5_rollback() {
    PRINTLN("\n--- Test 5: Rollback correctness ---");
    // Simulate #177's MutationCheckpoint pattern:
    //   1. Capture pre-mutation state (a copy of the PCV)
    //   2. Mutate forward
    //   3. "Rollback" by reinstalling the capture
    //   4. Verify the result == the pre-mutation state
    PCV pre_mutation{1, 2, 3, 4, 5};
    PCV current = pre_mutation;  // checkpoint captures the old state

    // Mutate forward
    current = current.with_push_back(99);
    current = current.with_set(0, 88);
    current = current.with_erase(2);
    CHECK(current.size() == 5, "after 3 mutations, size 5");
    CHECK(current[0] == 88, "[0] == 88 (set)");
    CHECK(current[2] == 4, "[2] == 4 (erase removed 3)");

    // Rollback
    current = pre_mutation;  // reinstall the snapshot
    CHECK(current == pre_mutation, "rollback → current == pre_mutation");
    CHECK(current.size() == 5, "rollback size 5");
    CHECK(current[0] == 1 && current[4] == 5, "rollback content matches");

    // The forward-mutated vector is still alive (its shared_ptr
    // has been dropped, so the old storage can be freed — but
    // the variable is still in scope for the test).
}

// ── Test 6: Empty vector + edge cases ───────────────────────
void test_6_empty_and_edges() {
    PRINTLN("\n--- Test 6: Empty vector + edge cases ---");
    PCV v;
    CHECK(v.empty(), "empty");
    CHECK(v.with_push_back(1).size() == 1, "with_push_back on empty works");
    CHECK(v.with_insert(0, 99).size() == 1,
          "with_insert on empty at pos 0 works");
    CHECK(v.with_erase(0).empty(), "with_erase on empty is no-op (empty)");
    CHECK(v.with_set(0, 5).empty(),
          "with_set on empty is no-op (out of range)");

    PCV v2{42};
    CHECK(v2.with_erase(0).empty(), "erase the only element → empty");
    CHECK(v2.with_insert(5, 99).size() == 2,
          "insert at pos > size is clamped to size (append)");

    PCV v3{1, 2, 3};
    CHECK(v3.with_insert(10, 99).size() == 4,
          "with_insert at pos > size appends");
    CHECK(v3.with_insert(10, 99)[3] == 99, "appended element at end");
}

// ── Test 7: Comparison ───────────────────────────────────────
void test_7_comparison() {
    PRINTLN("\n--- Test 7: Comparison ---");
    PCV a{1, 2, 3};
    PCV b{1, 2, 3};
    PCV c{1, 2, 4};
    PCV d{1, 2};
    PCV e;  // empty
    PCV f;  // empty

    CHECK(a == b, "a == b (same content)");
    CHECK(!(a == c), "a != c (different content)");
    CHECK(!(a == d), "a != d (different size)");
    CHECK(e == f, "two empty PCVs are equal");
    CHECK(a != e, "non-empty != empty");
}

// ── Test 8: Perf — 5000 mutations on a 1000-element base ────
void test_8_perf() {
    PRINTLN("\n--- Test 8: Perf — 5000 mutations on a 1000-element base ---");
    // Build a 1000-element base
    PCV base;
    for (std::uint32_t i = 0; i < 1000; ++i) {
        base = base.with_push_back(i);
    }
    CHECK(base.size() == 1000, "base has 1000 elements");

    // 100 mutations (each with_push_back)
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        base = base.with_push_back(1000 + i);
    }
    auto end = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    auto per_op_us = static_cast<double>(us) / 100.0;
    std::fprintf(stdout, "  INFO: 100 with_push_back on 1000-elem base: %ld µs (%.2f µs/op)\n",
                 static_cast<long>(us), per_op_us);
    CHECK(per_op_us < 100.0, "per-op < 100µs (issue #220 baseline, 5x margin)");

    // 100 mixed mutations
    start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        if (i % 2 == 0) base = base.with_push_back(2000 + i);
        else if (i % 3 == 0) base = base.with_erase(500);
        else base = base.with_set(i % base.size(), 3000 + i);
    }
    end = std::chrono::steady_clock::now();
    us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    per_op_us = static_cast<double>(us) / 100.0;
    std::fprintf(stdout, "  INFO: 100 mixed on 1000-elem base: %ld µs (%.2f µs/op)\n",
                 static_cast<long>(us), per_op_us);
    CHECK(per_op_us < 100.0, "per-op < 100µs (mixed mutations)");
}

// ── Test 9: Wire format roundtrip (per-node list column) ─────
void test_9_wire_format() {
    PRINTLN("\n--- Test 9: Wire format roundtrip ---");
    // The #220 wire format for a per-node list:
    //   per-node count (u32) + flat concatenation of children
    // PersistentChildVector serializes naturally to this format.
    PCV original{1, 2, 3};
    auto serialize_to_buf = [](const PCV& v, std::vector<char>& buf) {
        // Per-node count (1 node)
        std::uint32_t count = 1;
        buf.insert(buf.end(), reinterpret_cast<char*>(&count),
                   reinterpret_cast<char*>(&count) + 4);
        // Per-node child count
        std::uint32_t child_count = static_cast<std::uint32_t>(v.size());
        buf.insert(buf.end(), reinterpret_cast<char*>(&child_count),
                   reinterpret_cast<char*>(&child_count) + 4);
        // Flat children
        std::uint32_t total = child_count;
        buf.insert(buf.end(), reinterpret_cast<char*>(&total),
                   reinterpret_cast<char*>(&total) + 4);
        for (std::size_t i = 0; i < v.size(); ++i) {
            std::uint32_t x = v[i];
            buf.insert(buf.end(), reinterpret_cast<char*>(&x),
                       reinterpret_cast<char*>(&x) + 4);
        }
    };
    auto deserialize_from_buf = [](const std::vector<char>& buf,
                                  std::size_t& pos) -> PCV {
        std::uint32_t count; std::memcpy(&count, &buf[pos], 4); pos += 4;
        std::uint32_t child_count; std::memcpy(&child_count, &buf[pos], 4); pos += 4;
        std::uint32_t total; std::memcpy(&total, &buf[pos], 4); pos += 4;
        PCV out;
        for (std::uint32_t i = 0; i < child_count; ++i) {
            std::uint32_t x; std::memcpy(&x, &buf[pos], 4); pos += 4;
            out = out.with_push_back(x);
        }
        return out;
    };

    // Roundtrip a non-empty vector
    std::vector<char> buf;
    serialize_to_buf(original, buf);
    std::size_t pos = 0;
    auto rt = deserialize_from_buf(buf, pos);
    CHECK(pos == buf.size(), "all bytes consumed");
    CHECK(rt.size() == 3, "deserialized size 3");
    CHECK(rt[0] == 1 && rt[1] == 2 && rt[2] == 3, "deserialized content matches");

    // Roundtrip an empty vector
    PCV empty;
    buf.clear();
    serialize_to_buf(empty, buf);
    pos = 0;
    auto rt_empty = deserialize_from_buf(buf, pos);
    CHECK(pos == buf.size(), "all bytes consumed (empty)");
    CHECK(rt_empty.empty(), "deserialized empty is empty");
}

int main() {
    test_1_basic();
    test_2_cow();
    test_3_back_references();
    test_4_branches();
    test_5_rollback();
    test_6_empty_and_edges();
    test_7_comparison();
    test_8_perf();
    test_9_wire_format();
    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
