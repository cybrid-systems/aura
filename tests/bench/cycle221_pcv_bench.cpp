// tests/bench/cycle221_pcv_bench.cpp — Issue #221 PCV bench (Cycle 14 P5). (R18 moved from
// tests/core/.)
//
// Quantitative benchmark for the PersistentChildVector (PCV)
// path shipped in slices 1-4/5 of Issue #221. The PCV replaces
// the mutable std::pmr::vector<NodeId> children_ storage (added
// in #220) with an immutable + copy-on-write vector. This bench
// measures:
//
//   1. PCV ops perf — with_push_back / with_insert / with_erase /
//      with_set on a 1000-element base. Each `with_*` allocates a
//      new buffer + copies the old elements + applies the change.
//      Compare against an in-place std::vector<NodeId> (the legacy
//      path that PCV replaces).
//      Question: what is the PCV overhead vs in-place ops?
//
//   2. COW back-references — measure the shared_ptr refcount
//      growth when a single PCV is "forked" N times (each fork
//      is a with_push_back returning a new PCV). The original
//      PCV should see use_count() == N+1 (N forks + the
//      original).
//      Question: is the COW overhead amortized across many
//      forks? (Yes — the underlying storage is shared.)
//
//   3. #177 rollback flow — simulate a FlatASTStub with a
//      std::pmr::vector<PCV<NodeId>> children_ field. Enter a
//      "mutation boundary" (snapshot children_), do 10 mutations
//      (each replaces children_[id] with a new PCV), then
//      rollback by reinstalling the snapshot. Measure the
//      wall-clock cost of the boundary + rollback.
//      Question: is the rollback overhead acceptable for the
//      mutation boundary hot path?
//
//   4. Persistence stress — build a 5000-node "FlatASTStub"
//      (just a vector<PCV<NodeId>>), do 100 sequential
//      mutations, measure the average µs/op. The AC from
//      #221 is < 2µs/op.
//      Question: does the 5000-node + 100-mutations scenario
//      meet the AC?
//
// Output:
//   - Human-readable table to stdout
//   - JSON to tests/bench_results/cycle221_pcv_bench.json
//     for data-driven design decisions.
//
// Standalone TU (no module imports — avoids the GCC 16.1
// std module + P2996 reflection conflict). The PCV header
// is header-only. The "FlatASTStub" is a minimal simulation
// (just the children_ field + snapshot/restore) — sufficient
// for measuring the PCV-specific overhead, not for measuring
// the lock/mutex overhead in the real enter_mutation_boundary.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <print>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "core/persistent_child_vector.hh"

using PCV = aura::ast::PersistentChildVector<std::uint32_t>;
using NodeId = std::uint32_t;
static constexpr NodeId NULL_NODE = ~0u;

// ── Benchmark harness ──────────────────────────────────────────
struct BenchResult {
    std::string name;
    std::size_t n;
    double median_ms;
    double median_us_per_op = 0.0;
    std::size_t buf_bytes = 0;
};

template <typename Fn> double benchmark_median(Fn&& fn, int repeats = 5) {
    std::vector<double> samples;
    samples.reserve(repeats);
    for (int i = 0; i < repeats; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        fn();
        auto t1 = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2]; // median
}

// Build a base PCV of N elements [0, N).
static PCV make_base(std::size_t n) {
    std::vector<NodeId> elems(n);
    std::iota(elems.begin(), elems.end(), 0u);
    return PCV(elems.begin(), elems.end());
}

// Build an std::vector<NodeId> base of N elements [0, N).
static std::vector<NodeId> make_base_vec(std::size_t n) {
    std::vector<NodeId> v(n);
    std::iota(v.begin(), v.end(), 0u);
    return v;
}

// ── Bench 1: PCV ops perf (COW overhead) ───────────────────────
static void bench_pcv_ops(std::vector<BenchResult>& results) {
    std::println("\n── Bench 1: PCV ops perf (with_* vs in-place) ──");
    constexpr std::size_t BASE_N = 1000;
    constexpr std::size_t OPS = 1000;
    auto base_pcv = make_base(BASE_N);

    // 1a. with_push_back (always allocates new buffer of size N+1)
    double pcv_push_ms = benchmark_median([&] {
        PCV cur = base_pcv;
        for (std::size_t i = 0; i < OPS; ++i) {
            cur = cur.with_push_back(static_cast<NodeId>(BASE_N + i));
        }
    });
    double pcv_push_us = (pcv_push_ms * 1000.0) / OPS;

    // 1b. with_erase (allocates new buffer of size N-1)
    double pcv_erase_ms = benchmark_median([&] {
        PCV cur = base_pcv;
        for (std::size_t i = 0; i < OPS; ++i) {
            cur = cur.with_erase(i % cur.size());
        }
    });
    double pcv_erase_us = (pcv_erase_ms * 1000.0) / OPS;

    // 1c. with_insert at midpoint (allocates new buffer of size N+1)
    double pcv_insert_ms = benchmark_median([&] {
        PCV cur = base_pcv;
        for (std::size_t i = 0; i < OPS; ++i) {
            cur = cur.with_insert(cur.size() / 2, static_cast<NodeId>(BASE_N + i));
        }
    });
    double pcv_insert_us = (pcv_insert_ms * 1000.0) / OPS;

    // 1d. with_set (allocates new buffer of same size N)
    double pcv_set_ms = benchmark_median([&] {
        PCV cur = base_pcv;
        for (std::size_t i = 0; i < OPS; ++i) {
            cur = cur.with_set(i % cur.size(), static_cast<NodeId>(i * 7));
        }
    });
    double pcv_set_us = (pcv_set_ms * 1000.0) / OPS;

    // Comparison: in-place std::vector<NodeId> ops
    auto base_vec = make_base_vec(BASE_N);
    double vec_push_ms = benchmark_median([&] {
        std::vector<NodeId> cur = base_vec;
        for (std::size_t i = 0; i < OPS; ++i) {
            cur.push_back(static_cast<NodeId>(BASE_N + i));
        }
    });
    double vec_push_us = (vec_push_ms * 1000.0) / OPS;

    double vec_erase_ms = benchmark_median([&] {
        std::vector<NodeId> cur = base_vec;
        for (std::size_t i = 0; i < OPS; ++i) {
            cur.erase(cur.begin() + (i % cur.size()));
        }
    });
    double vec_erase_us = (vec_erase_ms * 1000.0) / OPS;

    double vec_set_ms = benchmark_median([&] {
        std::vector<NodeId> cur = base_vec;
        for (std::size_t i = 0; i < OPS; ++i) {
            cur[i % cur.size()] = static_cast<NodeId>(i * 7);
        }
    });
    double vec_set_us = (vec_set_ms * 1000.0) / OPS;

    std::println("  Base: {} elements, {} ops each", BASE_N, OPS);
    std::println("  ┌──────────────┬──────────────┬──────────────┬──────────┐");
    std::println("  │ Operation     │  PCV (µs/op) │ vec (µs/op) │   Ratio  │");
    std::println("  ├──────────────┼──────────────┼──────────────┼──────────┤");
    auto row = [](const char* name, double pcv_us, double vec_us) {
        double ratio = (vec_us > 0) ? (pcv_us / vec_us) : 0.0;
        std::println("  │ {:<12} │ {:>10.3f}   │ {:>10.3f}   │ {:>6.2f}x │", name, pcv_us, vec_us,
                     ratio);
    };
    row("push_back", pcv_push_us, vec_push_us);
    row("erase", pcv_erase_us, vec_erase_us);
    row("set", pcv_set_us, vec_set_us);
    std::println("  └──────────────┴──────────────┴──────────────┴──────────┘");
    std::println("  with_insert at midpoint: {:.3f} µs/op", pcv_insert_us);
    std::println("  (Note: with_insert copies two halves — no in-place vec midpoint comparison)");

    results.push_back({"pcv_push_back", OPS, pcv_push_ms, pcv_push_us, BASE_N * sizeof(NodeId)});
    results.push_back({"pcv_erase", OPS, pcv_erase_ms, pcv_erase_us, BASE_N * sizeof(NodeId)});
    results.push_back({"pcv_set", OPS, pcv_set_ms, pcv_set_us, BASE_N * sizeof(NodeId)});
    results.push_back({"vec_push_back", OPS, vec_push_ms, vec_push_us, BASE_N * sizeof(NodeId)});
    results.push_back({"vec_erase", OPS, vec_erase_ms, vec_erase_us, BASE_N * sizeof(NodeId)});
    results.push_back({"vec_set", OPS, vec_set_ms, vec_set_us, BASE_N * sizeof(NodeId)});
}

// ── Bench 2: COW back-references (refcount growth on fork) ─────
//
// In the production scenario (Issue #221), a closure captures a
// PCV pre-mutation (gets a PCV copy of children[id]), then the
// underlying children[id] is mutated to a NEW PCV (the closure's
// captured PCV is unchanged because PCVs are immutable). The
// closure's captured PCV continues to point at the pre-mutation
// storage as long as the closure holds it. That's the
// "back-reference" — a snapshot taken before the mutation
// outlives the mutation.
//
// To verify the COW semantics, we measure:
//   - base_copy.use_count() after copying the base (should be 2:
//     one for base, one for the copy).
//   - After with_push_back on the copy, base_copy's storage
//     refcount is UNCHANGED (because the copy's with_push_back
//     allocates a fresh storage for the new PCV; the copy is
//     rebound to the new storage).
//   - The original base's storage is unchanged throughout.
static void bench_cow_backrefs(std::vector<BenchResult>& results) {
    std::println("\n── Bench 2: COW back-references ──");
    constexpr std::size_t BASE_N = 100;
    auto base = make_base(BASE_N);
    long initial_use = base.use_count();
    std::println("  initial base.use_count() = {}", initial_use);

    // 2a. Copy the base. The copy shares storage with base.
    constexpr int COPIES[] = {1, 10, 100, 1000};
    for (int copies : COPIES) {
        std::vector<PCV> holders;
        holders.reserve(copies);
        long use_after_copy;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < copies; ++i) {
            holders.push_back(base); // PCV copy: shared_ptr refcount++
        }
        auto t1 = std::chrono::steady_clock::now();
        use_after_copy = base.use_count();
        double copy_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double copy_us = (copy_ms * 1000.0) / copies;

        // 2b. Now mutate ONE holder (with_push_back). The mutation
        // produces a new PCV with fresh storage. The mutated
        // holder's data_ is now bound to the NEW storage
        // (use_count of NEW storage == 1). The OTHER holders
        // (and base) still point to the original storage; their
        // shared_ptrs prevent the original storage from being
        // freed even though only the copies + base hold it.
        auto& one_holder = holders[0];
        long use_before_mutate = one_holder.use_count(); // copies + 1 (for base)
        one_holder = one_holder.with_push_back(999);
        long use_after_mutate_holder = one_holder.use_count(); // 1 (new storage)
        long use_after_mutate_base = base.use_count();         // copies (unaffected by the mutate)

        // Sanity: the original storage is still alive (its
        // use_count == copies, because base + the other
        // copies-then-not-mutated holders still hold it).
        if (use_after_mutate_base != copies) {
            std::fprintf(stderr, "  WARN: base.use_count() after mutate = %ld, expected %d\n",
                         use_after_mutate_base, copies);
        }
        std::println("  copies={:>5}  copy={:.3f} µs  base.use_count()={} (was {})  "
                     "after mutate: holder={}, base={}",
                     copies, copy_us, use_after_copy, initial_use, use_after_mutate_holder,
                     use_after_mutate_base);
        results.push_back({"cow_backref_copy", static_cast<std::size_t>(copies), copy_ms, copy_us,
                           BASE_N * sizeof(NodeId)});
        // Reset for next iteration
        holders.clear();
    }
    std::println("  → original storage outlives mutations; use_count tracks shared holders");
    std::println(
        "  → mutated holder rebinds to fresh storage (old storage kept alive by base + holders)");
}

// ── Bench 3: Simulated #177 rollback flow ──────────────────────
//
// Simulates the per-node children_ manipulation that
// enter_mutation_boundary + restore_children do. We measure
// just the vector manipulation cost (no mutex / no mutation
// log); the production enter/exit adds:
//
//   - shared_mutex lock/unlock (~50-100ns)
//   - mutation log size capture (~5ns)
//   - rollback_to_size (~50-200ns for typical log sizes)
//
// But the PCV-specific overhead (snapshot copy + restore
// move) is what we measure here.
struct FlatASTStub {
    // Mirror the production children_ field: std::pmr::vector<PCV<NodeId>>.
    // Default polymorphic_allocator so the inner PMR matches FlatAST's.
    std::vector<PCV> children_;
    void bump_generation() { /* no-op for bench */ }
};

static void bench_rollback_flow(std::vector<BenchResult>& results) {
    std::println("\n── Bench 3: Simulated #177 rollback flow ──");
    constexpr std::size_t NODE_COUNT = 5000; // 5000-node AST
    constexpr int BOUNDARIES = 100;
    constexpr int MUTATIONS_PER_BOUNDARY = 10;

    FlatASTStub stub;
    stub.children_.reserve(NODE_COUNT);
    // Build a 5000-node AST where each node has 3 children.
    std::vector<NodeId> base_kids = {1, 2, 3};
    for (std::size_t i = 0; i < NODE_COUNT; ++i) {
        stub.children_.push_back(PCV(base_kids.begin(), base_kids.end()));
    }

    // Run BOUNDARIES × MUTATIONS_PER_BOUNDARY mutations,
    // each followed by a rollback. This is the worst case
    // for the rollback path (every boundary aborts).
    auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < BOUNDARIES; ++b) {
        // enter_mutation_boundary: capture snapshot (PCV copy of children_)
        auto snapshot = stub.children_; // std::vector<PCV> copy = N shared_ptr copies
        // Mutations: replace 10 random children
        for (int m = 0; m < MUTATIONS_PER_BOUNDARY; ++m) {
            std::size_t idx = (b * MUTATIONS_PER_BOUNDARY + m) % NODE_COUNT;
            stub.children_[idx] = stub.children_[idx].with_push_back(
                static_cast<NodeId>(NODE_COUNT + b * MUTATIONS_PER_BOUNDARY + m));
        }
        // exit_mutation_boundary(false): rollback
        stub.children_ = std::move(snapshot); // move from snapshot back
        stub.bump_generation();
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double us_per_op = (ms * 1000.0) / (BOUNDARIES * MUTATIONS_PER_BOUNDARY);
    std::println("  {} nodes, {} boundaries × {} mutations = {} ops", NODE_COUNT, BOUNDARIES,
                 MUTATIONS_PER_BOUNDARY, BOUNDARIES * MUTATIONS_PER_BOUNDARY);
    std::println("  Total: {:.3f} ms ({:.3f} µs/op)", ms, us_per_op);
    std::println("  → includes snapshot copy + restore_children move + 10 mutations");
    results.push_back(
        {"rollback_flow", NODE_COUNT, ms, us_per_op, NODE_COUNT * 3 * sizeof(NodeId)});
}

// ── Bench 4: Persistence stress (5000 nodes + 100 mutations < 2µs/op) ─
static void bench_persistence_stress(std::vector<BenchResult>& results) {
    std::println("\n── Bench 4: Persistence stress (AC: < 2µs/op) ──");
    constexpr std::size_t NODE_COUNT = 5000;
    constexpr int MUTATIONS = 100;

    std::vector<PCV> children;
    children.reserve(NODE_COUNT);
    std::vector<NodeId> base_kids = {1, 2, 3};
    for (std::size_t i = 0; i < NODE_COUNT; ++i) {
        children.push_back(PCV(base_kids.begin(), base_kids.end()));
    }

    // Run MUTATIONS mutations, each a with_push_back on a
    // random node. The AC is < 2µs/op. We measure median
    // across 5 runs.
    std::mt19937 rng(42);
    std::uniform_int_distribution<std::size_t> dist(0, NODE_COUNT - 1);

    auto run = [&]() {
        std::vector<PCV> cur = children; // local copy
        for (int m = 0; m < MUTATIONS; ++m) {
            std::size_t idx = dist(rng);
            cur[idx] = cur[idx].with_push_back(static_cast<NodeId>(NODE_COUNT + m));
        }
    };
    double ms = benchmark_median(run);
    double us_per_op = (ms * 1000.0) / MUTATIONS;
    bool meets_ac = us_per_op < 2.0;
    std::println("  {} nodes, {} mutations per run", NODE_COUNT, MUTATIONS);
    std::println("  Median: {:.3f} µs/op  {}", us_per_op,
                 meets_ac ? "✅ MEETS AC" : "❌ EXCEEDS AC");
    results.push_back(
        {"persistence_stress", NODE_COUNT, ms, us_per_op, NODE_COUNT * 3 * sizeof(NodeId)});
}

// ── Main ───────────────────────────────────────────────────────
int main() {
    std::println("═══ Cycle 14 P5: PersistentChildVector benchmark suite ═══");
    std::println("Issue: #221 (Cycle 14 P5 — PCV benchmarks)");

    std::vector<BenchResult> results;
    bench_pcv_ops(results);
    bench_cow_backrefs(results);
    bench_rollback_flow(results);
    bench_persistence_stress(results);

    // ── JSON output ──
    std::filesystem::path out_dir = "tests/bench_results";
    std::filesystem::create_directories(out_dir);
    std::filesystem::path out_file = out_dir / "cycle221_pcv_bench.json";
    {
        std::ofstream f(out_file);
        f << "{\n";
        f << "  \"issue\": \"#221 Cycle 14 P5\",\n";
        f << "  \"date\": \"2026-06-16\",\n";
        f << "  \"results\": [\n";
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            f << "    {\"name\": \"" << r.name << "\", \"n\": " << r.n
              << ", \"median_ms\": " << std::fixed << std::setprecision(3) << r.median_ms
              << ", \"us_per_op\": " << std::setprecision(3) << r.median_us_per_op
              << ", \"buf_bytes\": " << r.buf_bytes << "}";
            if (i + 1 < results.size())
                f << ",";
            f << "\n";
        }
        f << "  ]\n";
        f << "}\n";
    }
    std::println("\n  JSON written to {}", out_file.string());
    return 0;
}
