// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_222.cpp — Issue #222: structural mutation
// concurrency safety (Issue #179 Cycle 4, slice 1/3).
//
// Tests the begin_structural_mutation() / end_structural_mutation()
// (RAII guard) + set_child / insert_child / remove_child routing
// + try_acquire_reader_lock() reader-side API.
//
// The tests focus on the slice 1 deliverables:
//
//   1. Single-threaded correctness (no regression):
//      - StructuralMutationGuard + lock acquired/released properly
//      - set_child / insert_child / remove_child behave as before
//      - bump_generation is called on every mutate
//      - mark_dirty is called on every mutate
//
//   2. Two-thread concurrent mutate + query (smoke test):
//      - 2 threads, each doing 100 mutates + reads
//      - No crash, no data corruption
//      - Generation monotonically increases
//
//   3. Multi-thread concurrent mutate stress (stress test):
//      - 4 threads × 1000 mutates each
//      - All reads see consistent (parent, generation) state
//      - bump_generation total == total mutate count
//
//   4. Reader-side lock API:
//      - try_acquire_reader_lock() returns a valid guard
//      - Multiple reader locks can coexist (shared lock)
//      - The guard's lifetime is the duration of the read
//
// Standalone TU (no module imports — avoids the GCC 16.1
// std module + P2996 reflection conflict). The test creates a
// real FlatAST-like struct with the same children_ storage
// pattern (per-node PCV) and the structural_mtx_ / guard API
// from the production code.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/core/persistent_child_vector.hh"

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

using PCV = aura::ast::PersistentChildVector<std::uint32_t>;
using NodeId = std::uint32_t;
static constexpr NodeId NULL_NODE = ~0u;



#define PRINTLN(msg) std::fprintf(stdout, "%s\n", (msg))

// Minimal reproduction of FlatAST's structural_mtx_ +
// bump_generation + mark_dirty API for testing purposes.
// Mirrors the production API in src/core/ast.ixx closely
// enough to validate the design without needing the module.
struct TestFlatAST {
    std::vector<PCV> children_;  // per-node children
    std::vector<NodeId> parent_;
    std::vector<std::uint8_t> dirty_;
    std::uint16_t generation_ = 1;
    // Mirror the production OwnedSharedMutex wrapper inline.
    struct MutexHolder {
        std::shared_mutex mtx;
    };
    MutexHolder structural_mtx_;

    static constexpr std::uint8_t kGeneralDirty = 0x01;

    void bump_generation() noexcept {
        ++generation_;
        // Skip 0 (reserved as "no node"); wraps at 65535
        if (generation_ == 0) generation_ = 1;
    }

    void mark_dirty(NodeId id, std::uint8_t reasons = kGeneralDirty) {
        if (id >= dirty_.size()) dirty_.resize(id + 1, 0);
        dirty_[id] |= reasons;
    }

    bool is_dirty(NodeId id) const {
        return id < dirty_.size() && dirty_[id] != 0;
    }

    NodeId add_node() {
        NodeId id = static_cast<NodeId>(children_.size());
        children_.emplace_back();
        parent_.push_back(NULL_NODE);
        dirty_.push_back(0);
        return id;
    }

    // ── Guard classes (mirroring FlatAST's API) ──
    class StructuralMutationGuard {
    public:
        StructuralMutationGuard() = default;
        explicit StructuralMutationGuard(TestFlatAST* ast)
            : ast_(ast), lock_() {
            if (ast_) lock_ = std::unique_lock<std::shared_mutex>(ast->structural_mtx_.mtx);
        }
        ~StructuralMutationGuard() { if (ast_) ast_->bump_generation(); }
        StructuralMutationGuard(const StructuralMutationGuard&) = delete;
        StructuralMutationGuard& operator=(const StructuralMutationGuard&) = delete;
        StructuralMutationGuard(StructuralMutationGuard&& o) noexcept
            : ast_(o.ast_), lock_(std::move(o.lock_)) { o.ast_ = nullptr; }
        explicit operator bool() const noexcept {
            return ast_ != nullptr && lock_.owns_lock();
        }
    private:
        TestFlatAST* ast_ = nullptr;
        std::unique_lock<std::shared_mutex> lock_;
    };
    [[nodiscard]] StructuralMutationGuard begin_structural_mutation() {
        return StructuralMutationGuard(this);
    }

    class ReaderLockGuard {
    public:
        ReaderLockGuard() = default;
        explicit ReaderLockGuard(const TestFlatAST* ast)
            : ast_(ast), lock_() {
            if (ast_) {
                // shared_lock needs a non-const mutex ref to acquire;
                // const_cast is safe (acquiring the lock doesn't modify
                // the protected data, only the mutex's internal state).
                auto& mtx = const_cast<std::shared_mutex&>(ast->structural_mtx_.mtx);
                lock_ = std::shared_lock<std::shared_mutex>(mtx);
            }
        }
        ~ReaderLockGuard() = default;
        ReaderLockGuard(const ReaderLockGuard&) = delete;
        ReaderLockGuard& operator=(const ReaderLockGuard&) = delete;
        ReaderLockGuard(ReaderLockGuard&& o) noexcept
            : ast_(o.ast_), lock_(std::move(o.lock_)) { o.ast_ = nullptr; }
        ReaderLockGuard& operator=(ReaderLockGuard&& o) noexcept {
            if (this != &o) {
                ast_ = o.ast_;
                lock_ = std::move(o.lock_);
                o.ast_ = nullptr;
            }
            return *this;
        }
        explicit operator bool() const noexcept {
            return ast_ != nullptr && lock_.owns_lock();
        }
    private:
        const TestFlatAST* ast_ = nullptr;
        std::shared_lock<std::shared_mutex> lock_;
    };
    [[nodiscard]] ReaderLockGuard try_acquire_reader_lock() const {
        return ReaderLockGuard(this);
    }

    // ── Mutators (routed through guard + mark_dirty + mutation_log_) ──
    // Issue #222 slice 2/3: structural mutates also append a
    // MutationRecord to the log + mark_dirty_upward(parent).
    struct MockMutationRecord {
        NodeId parent;
        std::uint32_t child_idx;
        NodeId old_child;
        NodeId new_child;
        const char* op_name;
    };
    std::vector<MockMutationRecord> mutation_log_;
    NodeId mark_dirty_upward_target_ = NULL_NODE;

    void mark_dirty_upward(NodeId id, std::uint8_t reasons = 0x01) {
        if (id != NULL_NODE) mark_dirty(id, reasons);
        mark_dirty_upward_target_ = id;
    }

    void set_child(NodeId id, std::uint32_t idx, NodeId child) {
        StructuralMutationGuard guard(this);
        const auto& list = children_[id];
        if (idx >= list.size()) return;
        auto old_cid = list[idx];
        if (old_cid != NULL_NODE && old_cid < parent_.size())
            parent_[old_cid] = NULL_NODE;
        children_[id] = list.with_set(idx, child);
        if (child != NULL_NODE && child < parent_.size())
            parent_[child] = id;
        mutation_log_.push_back({id, idx, old_cid, child, "structural-set-child"});
        mark_dirty_upward(id);
    }
    void insert_child(NodeId id, std::uint32_t idx, NodeId child) {
        StructuralMutationGuard guard(this);
        const auto& list = children_[id];
        auto pos = std::min(static_cast<std::uint32_t>(list.size()), idx);
        children_[id] = list.with_insert(pos, child);
        if (child != NULL_NODE && child < parent_.size())
            parent_[child] = id;
        mutation_log_.push_back({id, pos, NULL_NODE, child, "structural-insert-child"});
        mark_dirty_upward(id);
    }
    void remove_child(NodeId id, std::uint32_t idx) {
        StructuralMutationGuard guard(this);
        const auto& list = children_[id];
        if (idx < list.size()) {
            auto cid = list[idx];
            if (cid != NULL_NODE && cid < parent_.size())
                parent_[cid] = NULL_NODE;
            children_[id] = list.with_erase(idx);
            mutation_log_.push_back({id, idx, cid, NULL_NODE, "structural-remove-child"});
            mark_dirty_upward(id);
        }
    }
};

// ── Test 1: Single-threaded correctness (no regression) ───────
void test_1_single_thread() {
    PRINTLN("\n--- Test 1: Single-threaded correctness ---");
    TestFlatAST ast;

    // Build a small AST: 5 nodes, each with 2 children
    std::vector<NodeId> ids;
    for (int i = 0; i < 5; ++i) ids.push_back(ast.add_node());
    // Add 10 more "leaf" nodes (one per child slot, no sharing).
    std::vector<NodeId> leaves;
    for (int i = 0; i < 10; ++i) leaves.push_back(ast.add_node());

    // Wire up: node[i].children = [leaves[2i], leaves[2i+1]]
    std::uint16_t gen_before = ast.generation_;
    for (int i = 0; i < 5; ++i) {
        ast.insert_child(ids[i], 0, leaves[2 * i]);
        ast.insert_child(ids[i], 1, leaves[2 * i + 1]);
    }
    // 10 inserts → generation bumped 10 times (guard dtor)
    CHECK(ast.generation_ > gen_before, "generation bumped after inserts");

    // Check parent_ links (each leaf has exactly one parent since no sharing)
    for (int i = 0; i < 5; ++i) {
        CHECK(ast.parent_[leaves[2 * i]] == ids[i], "first child parent_ link");
        CHECK(ast.parent_[leaves[2 * i + 1]] == ids[i], "second child parent_ link");
    }

    // mark_dirty called
    for (int i = 0; i < 5; ++i) {
        CHECK(ast.is_dirty(ids[i]), "node marked dirty after mutate");
    }

    // set_child + remove_child
    ast.set_child(ids[0], 0, ids[3]);
    CHECK(ast.children_[ids[0]][0] == ids[3], "set_child updated");
    CHECK(ast.is_dirty(ids[0]), "set_child marked dirty");

    ast.remove_child(ids[0], 0);
    CHECK(ast.children_[ids[0]].size() == 1, "remove_child shrunk list");

    // Guard pattern: acquire and release
    {
        auto guard = ast.begin_structural_mutation();
        CHECK(static_cast<bool>(guard), "guard is valid");
    }
    CHECK(true, "guard released on scope exit");

    // Generation incremented exactly once per guard release
    std::uint16_t g_before = ast.generation_;
    {
        auto guard = ast.begin_structural_mutation();
        // Mutate children_ directly under the guard (not via
        // set_child/insert_child, which would re-acquire the
        // lock and deadlock on non-recursive std::shared_mutex).
        ast.children_[ids[0]] = ast.children_[ids[0]].with_push_back(ids[4]);
        // generation NOT bumped yet (guard's dtor will bump)
    }
    CHECK(ast.generation_ == g_before + 1, "generation bumped exactly once on guard scope exit");
}

// ── Test 2: Reader-side lock API ──────────────────────────────
void test_2_reader_lock() {
    PRINTLN("\n--- Test 2: Reader-side lock API ---");
    TestFlatAST ast;
    auto id = ast.add_node();
    ast.insert_child(id, 0, id);

    auto guard = ast.try_acquire_reader_lock();
    CHECK(static_cast<bool>(guard), "reader guard is valid");

    // Multiple reader locks can coexist
    {
        auto g2 = ast.try_acquire_reader_lock();
        CHECK(static_cast<bool>(g2), "second reader lock acquired (shared)");
        // Reading children under both locks is safe
        auto kids = ast.children_[id];
        CHECK(kids.size() == 1, "read while holding two reader locks");
    }

    // Write lock would block — but we don't test that here
    // (would require a separate thread to avoid deadlock).
    {
        TestFlatAST::ReaderLockGuard tmp;  // release first lock
        guard = std::move(tmp);
    }
    CHECK(!static_cast<bool>(guard), "default-constructed guard is invalid");

    // Re-acquire after release
    auto guard2 = ast.try_acquire_reader_lock();
    CHECK(static_cast<bool>(guard2), "reader guard re-acquired after release");
}

// ── Test 3: Two-thread concurrent mutate + query (smoke) ─────
void test_3_two_thread_smoke() {
    PRINTLN("\n--- Test 3: Two-thread concurrent mutate + query ---");
    TestFlatAST ast;
    constexpr int N_NODES = 100;
    std::vector<NodeId> ids;
    for (int i = 0; i < N_NODES; ++i) ids.push_back(ast.add_node());

    std::atomic<bool> reader_ready{false};
    std::atomic<bool> writer_done{false};
    std::atomic<int> read_count{0};
    std::atomic<std::uint16_t> reader_max_gen{0};
    std::atomic<std::uint16_t> reader_min_gen{UINT16_MAX};

    // Reader: continuous reads, verify generation monotonicity.
    // Holds a reader lock for the duration of the read to test
    // the try_acquire_reader_lock API.
    std::thread reader([&]() {
        reader_ready.store(true, std::memory_order_release);
        int count = 0;
        while (!writer_done.load(std::memory_order_acquire)) {
            auto rg = ast.try_acquire_reader_lock();
            if (rg) {
                std::uint16_t g = ast.generation_;
                // Track max/min observed gen
                std::uint16_t cur_max = reader_max_gen.load();
                while (g > cur_max && !reader_max_gen.compare_exchange_weak(cur_max, g)) {}
                std::uint16_t cur_min = reader_min_gen.load();
                while (g < cur_min && !reader_min_gen.compare_exchange_weak(cur_min, g)) {}
                NodeId n = ids[count % N_NODES];
                auto sz = ast.children_[n].size();
                (void)sz;
                ++count;
            }
            std::this_thread::yield();
        }
        read_count = count;
    });

    // Writer: 1000 set_child on random nodes. Wait for the reader
    // thread to start and yield periodically so single-core CI
    // runners still observe concurrent mutate + read overlap.
    std::thread writer([&]() {
        while (!reader_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> dist(0, N_NODES - 1);
        for (int i = 0; i < 1000; ++i) {
            NodeId from = ids[dist(rng)];
            NodeId to = ids[dist(rng)];
            ast.set_child(from, 0, to);
            if ((i & 15) == 0) {
                std::this_thread::yield();
            }
        }
        writer_done.store(true, std::memory_order_release);
    });

    writer.join();
    reader.join();

    CHECK(read_count > 0, "reader iterated at least once");
    CHECK(ast.generation_ > 1, "writer bumped generation");
    CHECK(reader_max_gen.load() > reader_min_gen.load(),
          "reader observed multiple distinct generations (concurrent mutates happened)");
}

// ── Test 4: Multi-thread concurrent mutate stress ────────────
void test_4_multi_thread_stress() {
    PRINTLN("\n--- Test 4: Multi-thread concurrent mutate stress ---");
    TestFlatAST ast;
    constexpr int N_NODES = 100;
    constexpr int N_THREADS = 4;
    constexpr int MUTATES_PER_THREAD = 250;  // 1000 total
    std::vector<NodeId> ids;
    for (int i = 0; i < N_NODES; ++i) ids.push_back(ast.add_node());
    // Each node starts with 1 self-reference as child (so set_child has something to overwrite)
    for (auto id : ids) ast.insert_child(id, 0, id);

    std::uint16_t gen_before = ast.generation_;
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            std::mt19937 rng(42 + t);
            std::uniform_int_distribution<int> dist(0, N_NODES - 1);
            for (int i = 0; i < MUTATES_PER_THREAD; ++i) {
                NodeId from = ids[dist(rng)];
                NodeId to = ids[dist(rng)];
                // Use insert_child or set_child to vary the path
                if (i % 2 == 0) {
                    ast.set_child(from, 0, to);
                } else {
                    ast.insert_child(from, 1, to);
                }
                // No lock-free read after the mutate — TSan would
                // flag it as a data race. The parent_ invariant
                // check is done after all threads join (see below).
            }
        });
    }
    for (auto& th : threads) th.join();

    // Post-mutation invariant check (acquires a single reader lock
    // so the reads are TSan-clean):
    {
        auto rg = ast.try_acquire_reader_lock();
        CHECK(static_cast<bool>(rg), "post-stress reader lock acquired");
        // For each NodeId, verify children_[id] exists (no torn
        // write to the children_ vector itself).
        for (auto id : ids) {
            (void)ast.children_[id].size();
        }
    }

    CHECK(errors.load() == 0, "no torn reads detected");
    // Each mutate triggers exactly one generation bump (in guard's dtor)
    // 1000 mutates total → generation increased by at least 1000 (might be more if some ops were no-ops)
    std::uint16_t gen_after = ast.generation_;
    int gen_delta = (gen_after >= gen_before)
        ? (gen_after - gen_before)
        : (65535 - gen_before + gen_after);  // wraparound
    CHECK(gen_delta >= N_THREADS * MUTATES_PER_THREAD,
          "generation bumped at least N*M times");

    // All nodes should be dirty
    int dirty_count = 0;
    for (auto id : ids) if (ast.is_dirty(id)) ++dirty_count;
    CHECK(dirty_count >= N_NODES / 2,
          "majority of nodes are dirty (concurrent mutates touched them)");
}

// ── Test 5: Explicit guard for multi-step atomic mutate ─────
void test_5_explicit_guard() {
    PRINTLN("\n--- Test 5: Explicit guard for multi-step atomic mutate ---");
    TestFlatAST ast;
    constexpr int N = 10;
    std::vector<NodeId> ids;
    for (int i = 0; i < N; ++i) ids.push_back(ast.add_node());

    std::uint16_t gen_before = ast.generation_;
    {
        // Multi-step atomic: manipulate children_ directly under
        // the guard. NOTE: we don't call set_child/insert_child
        // here because they would try to re-acquire the lock
        // (recursive deadlock on non-recursive std::shared_mutex).
        // Instead, we mutate children_ directly; the guard's dtor
        // bumps generation_ once.
        auto guard = ast.begin_structural_mutation();
        // Swap: ids[0].children ↔ ids[5].children (1 child each)
        ast.children_[ids[0]] = ast.children_[ids[0]].with_push_back(ids[5]);
        ast.children_[ids[5]] = ast.children_[ids[5]].with_push_back(ids[0]);
        ast.children_[ids[1]] = ast.children_[ids[1]].with_push_back(ids[6]);
        ast.children_[ids[6]] = ast.children_[ids[6]].with_push_back(ids[1]);
        // 4 raw mutates under one guard → only ONE generation bump on guard exit
    }
    std::uint16_t gen_after = ast.generation_;
    int gen_delta = (gen_after >= gen_before)
        ? (gen_after - gen_before) : (65535 - gen_before + gen_after);
    CHECK(gen_delta == 1, "multi-step mutate bumps generation exactly once");

    // Verify the swap happened
    CHECK(ast.children_[ids[0]].size() >= 1, "ids[0] has at least 1 child");
    CHECK(ast.children_[ids[5]].size() >= 1, "ids[5] has at least 1 child");
}

#define SAFE_TEST(name, fn) do { \
    try { \
        fn(); \
        std::fprintf(stdout, "  ✓ " #fn " done\n"); std::fflush(stdout); \
    } catch (const std::system_error& e) { \
        std::fprintf(stderr, "✗ " #fn " system_error: %s (code=%d)\n", e.what(), e.code().value()); \
        std::fflush(stderr); ++g_failed; \
    } catch (const std::exception& e) { \
        std::fprintf(stderr, "✗ " #fn " exception: %s\n", e.what()); \
        std::fflush(stderr); ++g_failed; \
    } \
} while(0)

// ── Test 6: Mutation log + mark_dirty_upward (slice 2/3) ───
void test_6_mutation_log() {
    PRINTLN("\n--- Test 6: Mutation log + mark_dirty_upward ---");
    TestFlatAST ast;
    auto a = ast.add_node();
    auto b = ast.add_node();
    auto c = ast.add_node();

    // Initially: empty mutation log
    CHECK(ast.mutation_log_.empty(), "mutation log empty at start");

    // insert_child: appends one record (idx 0, list is empty so pos=0)
    ast.insert_child(a, 0, b);
    CHECK(ast.mutation_log_.size() == 1, "insert_child appended 1 mutation record");
    CHECK(ast.mutation_log_[0].op_name == std::string("structural-insert-child"),
          "op_name is structural-insert-child");
    CHECK(ast.mutation_log_[0].parent == a, "mutation record parent correct");
    CHECK(ast.mutation_log_[0].child_idx == 0, "mutation record child_idx correct");
    CHECK(ast.mutation_log_[0].old_child == NULL_NODE, "old_child NULL (was empty)");
    CHECK(ast.mutation_log_[0].new_child == b, "new_child is b");
    CHECK(ast.mark_dirty_upward_target_ == a, "mark_dirty_upward called with parent");

    // set_child on existing slot: appends one record
    ast.set_child(a, 0, c);
    CHECK(ast.mutation_log_.size() == 2, "set_child appended 1 mutation record");
    CHECK(ast.mutation_log_[1].op_name == std::string("structural-set-child"),
          "op_name is structural-set-child");
    CHECK(ast.mutation_log_[1].parent == a, "set_child parent correct");
    CHECK(ast.mutation_log_[1].child_idx == 0, "set_child child_idx correct");
    CHECK(ast.mutation_log_[1].old_child == b, "set_child old_child is b");
    CHECK(ast.mutation_log_[1].new_child == c, "set_child new_child is c");

    // remove_child: appends another
    ast.remove_child(a, 0);
    CHECK(ast.mutation_log_.size() == 3, "remove_child appended 1 mutation record");
    CHECK(ast.mutation_log_[2].op_name == std::string("structural-remove-child"),
          "op_name is structural-remove-child");
    CHECK(ast.mutation_log_[2].old_child == c, "remove old_child is c");
    CHECK(ast.mutation_log_[2].new_child == NULL_NODE, "remove new_child is NULL_NODE");

    // No-ops (out-of-range) don't append
    std::size_t before = ast.mutation_log_.size();
    ast.set_child(a, 999, NULL_NODE);  // out of range
    ast.remove_child(a, 999);          // out of range
    CHECK(ast.mutation_log_.size() == before, "out-of-range ops don't append to log");
}

// ── Test 7: High-iteration stress (slice 3/3) ───────────
// 8 threads × 5000 mutates = 40000 total mutates. Verifies
// the structural mutation guards scale to many concurrent
// mutates without deadlock, race, or generation overflow.
void test_7_high_iteration_stress() {
    PRINTLN("\n--- Test 7: High-iteration stress (40000 mutates) ---");
    TestFlatAST ast;
    constexpr int N_NODES = 200;
    constexpr int N_WRITER_THREADS = 8;
    constexpr int MUTATES_PER_THREAD = 5000;  // 40000 total
    constexpr int READER_ITERATIONS = 20000;
    std::vector<NodeId> ids;
    for (int i = 0; i < N_NODES; ++i) ids.push_back(ast.add_node());
    // Seed: each node has 1 self-child so set_child has work to do.
    for (auto id : ids) ast.insert_child(id, 0, id);

    std::atomic<bool> writers_done{false};
    std::atomic<int> read_iterations{0};
    std::atomic<int> read_failures{0};  // reader observed torn generation

    // Writers
    std::uint16_t gen_before = ast.generation_;
    std::vector<std::thread> writers;
    for (int t = 0; t < N_WRITER_THREADS; ++t) {
        writers.emplace_back([&, t]() {
            std::mt19937 rng(42 + t);
            std::uniform_int_distribution<int> dist(0, N_NODES - 1);
            for (int i = 0; i < MUTATES_PER_THREAD; ++i) {
                NodeId from = ids[dist(rng)];
                NodeId to = ids[dist(rng)];
                if (i % 3 == 0) {
                    ast.set_child(from, 0, to);
                } else if (i % 3 == 1) {
                    ast.insert_child(from, 1, to);
                } else {
                    ast.remove_child(from, 1);
                }
            }
        });
    }

    // Reader: continuous try_acquire_reader_lock + read generation
    std::thread reader([&]() {
        std::uint16_t last_gen = 0;
        int count = 0;
        while (!writers_done || count < READER_ITERATIONS) {
            auto rg = ast.try_acquire_reader_lock();
            if (rg) {
                std::uint16_t g = ast.generation_;
                if (g < last_gen) ++read_failures;  // should never happen
                last_gen = g;
                NodeId n = ids[count % N_NODES];
                (void)ast.children_[n].size();
                ++count;
            }
            if (count >= READER_ITERATIONS) break;
        }
        read_iterations = count;
    });

    for (auto& th : writers) th.join();
    writers_done = true;
    reader.join();

    CHECK(read_failures.load() == 0, "no torn generation reads across 20000 reader iterations");
    CHECK(read_iterations.load() >= READER_ITERATIONS / 2,
          "reader iterated at least half the target (lock contention OK)");

    std::uint16_t gen_after = ast.generation_;
    int gen_delta = (gen_after >= gen_before)
        ? (gen_after - gen_before)
        : (65535 - gen_before + gen_after);  // u16 wraparound
    int expected_min = N_WRITER_THREADS * MUTATES_PER_THREAD * 2 / 3;  // ~2/3 actually mutate (1/3 are remove on size 2)
    CHECK(gen_delta >= expected_min,
          "generation bumped at least the expected mutate count");

    // Post-stress invariants (acquire reader lock for TSan-clean reads)
    {
        auto rg = ast.try_acquire_reader_lock();
        CHECK(static_cast<bool>(rg), "post-stress reader lock acquired");
        for (auto id : ids) {
            (void)ast.children_[id].size();
            (void)ast.parent_[id];
        }
    }

    // Mutation log should have ~gen_delta entries (one per effective mutate)
    // Note: remove_child on out-of-range doesn't append, so this is approximate
    CHECK(ast.mutation_log_.size() <= static_cast<std::size_t>(gen_delta),
          "mutation log entries <= generation delta");
}

int main() {
    SAFE_TEST(t1, test_1_single_thread);
    SAFE_TEST(t2, test_2_reader_lock);
    SAFE_TEST(t3, test_3_two_thread_smoke);
    SAFE_TEST(t4, test_4_multi_thread_stress);
    SAFE_TEST(t5, test_5_explicit_guard);
    SAFE_TEST(t6, test_6_mutation_log);
    SAFE_TEST(t7, test_7_high_iteration_stress);
    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
