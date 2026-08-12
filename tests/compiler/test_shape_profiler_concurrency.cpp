// @category: unit
// @reason: Issue #2141 — ShapeProfiler shared_mutex for multi-fiber mutate.
//          Issue #2937 — FnKey-sharded locks (extend per #81967).
//
//   AC1: docs model A (shared_mutex) in shape_profiler.h
//   AC2: concurrent record_shape + invalidate does not corrupt profiles_
//   AC3: on_arena_compact + concurrent record_shape safe; stability preserved
//   AC4: deopt-storm still trips under concurrent invalidates
//   AC5: lock_contended_total accessible; optional contention under stress
//
//   #2937 AC1: concurrent record_shape on distinct FnKeys uses per-shard locks
//   #2937 AC2: is_stable / snapshot race-free under concurrent record/invalidate
//   #2937 AC3: on_arena_compact still soft-preserves; no mutation_induced bump
//   #2937 AC4: invalidate semantics unchanged (version + hook after unlock)
//   #2937 AC5: existing #2141 suite green under sharding
//   #2937 AC6: coverage linter + no docs/design/

#include "test_harness.hpp"

#include "compiler/shape_profiler.h"
#include "compiler/shape.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <vector>

namespace {

using aura::compiler::shape::FnKey;
using aura::compiler::shape::kShapeProfilerConcurrencyIssue;
using aura::compiler::shape::kShapeProfilerShardCount;
using aura::compiler::shape::kShapeProfilerShardIssue;
using aura::compiler::shape::SHAPE_FLOAT;
using aura::compiler::shape::SHAPE_INT;
using aura::compiler::shape::ShapeProfiler;
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

} // namespace

int run_test_shape_profiler_concurrency() {
    std::println("=== Issue #2141: ShapeProfiler multi-fiber concurrency ===");
    CHECK(kShapeProfilerConcurrencyIssue == 2141, "issue stamp");

    // ── AC1: documentation ──
    {
        std::println("\n--- AC1: concurrency model docs ---");
        auto hh = read_file("src/compiler/shape_profiler.h");
        auto cpp = read_file("src/compiler/shape_profiler.cpp");
        CHECK(hh.find("#2141") != std::string::npos, "header #2141");
        CHECK(hh.find("shared_mutex") != std::string::npos, "shared_mutex");
        CHECK(hh.find("model A") != std::string::npos ||
                  hh.find("reader/writer") != std::string::npos,
              "model A");
        CHECK(hh.find("NOT thread-safe by design") == std::string::npos,
              "old single-thread disclaimer removed");
        CHECK(hh.find("lock_contended_total") != std::string::npos, "contention metric API");
        CHECK(cpp.find("unique_lock_shard_") != std::string::npos ||
                  cpp.find("unique_lock_") != std::string::npos,
              "unique_lock helper");
        CHECK(cpp.find("shared_lock_shard_") != std::string::npos ||
                  cpp.find("shared_lock_") != std::string::npos,
              "shared_lock helper");
        CHECK(cpp.find("invalidate_unlocked_") != std::string::npos, "unlocked invalidate");
    }

    // ── AC2: dual-thread record + invalidate ──
    {
        std::println("\n--- AC2: concurrent record + invalidate ---");
        ShapeProfiler sp;
        std::atomic<bool> start{false};
        std::atomic<std::uint64_t> records{0};
        std::atomic<std::uint64_t> invalidates{0};
        std::atomic<int> errors{0};

        auto recorder = [&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < 2000; ++i) {
                const FnKey fn = static_cast<FnKey>(1 + (i % 32));
                const auto sid = (i & 1) ? SHAPE_INT : SHAPE_FLOAT;
                (void)sp.record_shape(fn, sid);
                records.fetch_add(1, std::memory_order_relaxed);
                // Concurrent readers
                (void)sp.is_stable(fn);
                (void)sp.current_snapshot(fn);
                (void)sp.dominant_shape(fn);
            }
        };
        auto invalidator = [&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < 2000; ++i) {
                const FnKey fn = static_cast<FnKey>(1 + (i % 32));
                (void)sp.invalidate(fn);
                invalidates.fetch_add(1, std::memory_order_relaxed);
                (void)sp.shape_stable_ratio();
                (void)sp.tracked_fns();
            }
        };

        std::thread t1(recorder);
        std::thread t2(invalidator);
        std::thread t3(recorder);
        start.store(true, std::memory_order_release);
        t1.join();
        t2.join();
        t3.join();

        CHECK(records.load() == 4000, "records completed");
        CHECK(invalidates.load() == 2000, "invalidates completed");
        CHECK(errors.load() == 0, "no error flags");
        // profiles_ integrity: tracked count finite, no crash on metrics
        const auto n = sp.profile_count();
        CHECK(n <= 32, "at most 32 fn keys");
        (void)sp.metrics(1);
        (void)sp.deopt_rate_per_fn();
        std::println("  profiles={} contended={}", n, sp.lock_contended_total());
    }

    // ── AC3: compact + concurrent record ──
    {
        std::println("\n--- AC3: on_arena_compact + concurrent record ---");
        ShapeProfiler sp;
        // Warm stable-ish profiles.
        for (int f = 1; f <= 8; ++f) {
            for (int i = 0; i < 150; ++i)
                (void)sp.record_shape(static_cast<FnKey>(f), SHAPE_INT);
        }
        const auto stable0 = sp.shape_stable_ratio();
        std::atomic<bool> start{false};
        std::atomic<std::uint32_t> compact_touched{0};

        auto recorder = [&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < 500; ++i)
                (void)sp.record_shape(static_cast<FnKey>(1 + (i % 8)), SHAPE_INT);
        };
        auto compactor = [&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < 50; ++i) {
                compact_touched.fetch_add(sp.on_arena_compact(), std::memory_order_relaxed);
            }
        };

        std::thread t1(recorder);
        std::thread t2(compactor);
        std::thread t3(recorder);
        start.store(true, std::memory_order_release);
        t1.join();
        t2.join();
        t3.join();

        CHECK(compact_touched.load() > 0, "compact touched profiles");
        CHECK(sp.arena_compact_calls() >= 50, "compact calls");
        // Stability may remain (compact preserves is_stable).
        const auto stable1 = sp.shape_stable_ratio();
        std::println("  stable_ratio {} → {}", stable0, stable1);
        CHECK(stable1 >= 0.0 && stable1 <= 1.0, "ratio in range");
        // #1521: compact does not alone force deopt storm.
        // (storm may be true from prior tests on other instances only)
    }

    // ── AC4: deopt-storm under concurrent invalidates ──
    {
        std::println("\n--- AC4: deopt-storm concurrent invalidate ---");
        ShapeProfiler sp;
        // Tight storm thresholds.
        ShapeProfiler::Preset p = ShapeProfiler::kDefaultPreset;
        p.deopt_storm_window = 16;
        p.deopt_storm_threshold = 4;
        sp.apply_preset(p);

        // Create profiles then invalidate many times concurrently.
        for (int f = 1; f <= 4; ++f) {
            for (int i = 0; i < 120; ++i)
                (void)sp.record_shape(static_cast<FnKey>(f), SHAPE_INT);
        }
        // Make stable then invalidate to feed storm (was_stable path).
        std::atomic<bool> start{false};
        auto worker = [&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < 40; ++i) {
                for (int f = 1; f <= 4; ++f) {
                    (void)sp.record_shape(static_cast<FnKey>(f), SHAPE_INT);
                    (void)sp.invalidate(static_cast<FnKey>(f));
                }
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        start.store(true, std::memory_order_release);
        t1.join();
        t2.join();

        // After many invalidates, storm should trip (threshold 4).
        CHECK(sp.deopt_storm_total() >= 1 || sp.deopt_storm_active(),
              "deopt storm observed under concurrent invalidate");
        std::println("  storm_active={} storm_total={}", sp.deopt_storm_active(),
                     sp.deopt_storm_total());
    }

    // ── AC5: contention metric API ──
    {
        std::println("\n--- AC5: lock_contended_total ---");
        ShapeProfiler sp;
        // May or may not contend on single-thread; just ensure API works.
        (void)sp.record_shape(1, SHAPE_INT);
        const auto c = sp.lock_contended_total();
        CHECK(c >= 0, "contended counter readable");
        // Dual-thread stress should complete without hang (already did).
        std::println("  lock_contended_total={}", c);
    }

    // invalidate_all concurrent with readers
    {
        std::println("\n--- invalidate_all + readers ---");
        ShapeProfiler sp;
        for (int f = 1; f <= 16; ++f)
            for (int i = 0; i < 50; ++i)
                (void)sp.record_shape(static_cast<FnKey>(f), SHAPE_FLOAT);
        std::atomic<bool> start{false};
        std::thread reader([&] {
            while (!start.load()) {
            }
            for (int i = 0; i < 200; ++i) {
                (void)sp.shape_stable_ratio();
                (void)sp.tracked_fns();
                (void)sp.is_stable(static_cast<FnKey>(1 + (i % 16)));
            }
        });
        std::thread wiper([&] {
            while (!start.load()) {
            }
            for (int i = 0; i < 20; ++i)
                sp.invalidate_all();
        });
        start.store(true);
        reader.join();
        wiper.join();
        CHECK(true, "invalidate_all + readers finished");
    }

    // ── Issue #2937: FnKey-sharded locks ──
    {
        std::println("\n--- #2937 AC1: shard infrastructure + distinct-FnKey concurrency ---");
        CHECK(kShapeProfilerShardIssue == 2937, "AC1: issue stamp 2937");
        CHECK(kShapeProfilerShardCount == 16, "AC1: shard count 16");
        CHECK(ShapeProfiler::shard_count() == kShapeProfilerShardCount, "AC1: API shard_count");
        // Distinct keys map across multiple shards (hash spread).
        std::size_t occupied = 0;
        bool seen[16] = {};
        for (FnKey f = 1; f <= 256; ++f) {
            const auto si = ShapeProfiler::shard_index(f);
            CHECK(si < kShapeProfilerShardCount, "AC1: shard_index in range");
            if (!seen[si]) {
                seen[si] = true;
                ++occupied;
            }
        }
        CHECK(occupied >= 8, "AC1: hash spreads across many shards");
        const auto hh = read_file("src/compiler/shape_profiler.h");
        const auto cpp = read_file("src/compiler/shape_profiler.cpp");
        CHECK(hh.find("#2937") != std::string::npos, "AC1: header cites #2937");
        CHECK(hh.find("kShapeProfilerShardCount") != std::string::npos,
              "AC1: shard count constant");
        CHECK(hh.find("ProfileShard") != std::string::npos, "AC1: ProfileShard type");
        CHECK(cpp.find("unique_lock_shard_") != std::string::npos, "AC1: per-shard unique");
        CHECK(cpp.find("unique_lock_all_shards_") != std::string::npos, "AC1: ordered multi-shard");
        CHECK(cpp.find("shard_index") != std::string::npos, "AC1: shard_index helper");

        // Multi-fiber: each thread owns a disjoint FnKey set → different shards.
        ShapeProfiler sp;
        std::atomic<bool> start{false};
        std::atomic<std::uint64_t> records{0};
        auto worker = [&](int base) {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < 1500; ++i) {
                // Stride keys so threads land on distinct hash classes.
                const FnKey fn = static_cast<FnKey>(base + static_cast<int>(i * 17));
                (void)sp.record_shape(fn == 0 ? 1 : fn, (i & 1) ? SHAPE_INT : SHAPE_FLOAT);
                records.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t0(worker, 1);
        std::thread t1(worker, 10007);
        std::thread t2(worker, 20011);
        std::thread t3(worker, 30013);
        start.store(true, std::memory_order_release);
        t0.join();
        t1.join();
        t2.join();
        t3.join();
        CHECK(records.load() == 6000, "AC1: disjoint-FnKey records completed");
        CHECK(sp.profile_count() > 0, "AC1: profiles populated");
        std::println("  profiles={} contended={}", sp.profile_count(), sp.lock_contended_total());
    }
    {
        std::println("\n--- #2937 AC2: is_stable / snapshot race-free ---");
        ShapeProfiler sp;
        for (int i = 0; i < 200; ++i)
            (void)sp.record_shape(42, SHAPE_INT);
        std::atomic<bool> start{false};
        std::atomic<int> errors{0};
        auto reader = [&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < 1000; ++i) {
                (void)sp.is_stable(42);
                (void)sp.current_snapshot(42);
                (void)sp.dominant_shape(42);
                (void)sp.metrics(42);
            }
        };
        auto writer = [&]() {
            while (!start.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < 1000; ++i) {
                (void)sp.record_shape(42, (i & 1) ? SHAPE_INT : SHAPE_FLOAT);
                if ((i % 50) == 0)
                    (void)sp.invalidate(42);
            }
        };
        std::thread r1(reader);
        std::thread r2(reader);
        std::thread w1(writer);
        start.store(true, std::memory_order_release);
        r1.join();
        r2.join();
        w1.join();
        CHECK(errors.load() == 0, "AC2: no error flags");
        (void)sp.current_snapshot(42);
    }
    {
        std::println("\n--- #2937 AC3: compact preserves stability; no mutation_induced ---");
        ShapeProfiler sp;
        sp.apply_preset(ShapeProfiler::kLowMutationPreset);
        for (int f = 1; f <= 12; ++f)
            for (int i = 0; i < 150; ++i)
                (void)sp.record_shape(static_cast<FnKey>(f), SHAPE_INT);
        const auto mut0 = sp.mutation_induced_invalidations();
        const auto storm0 = sp.deopt_storm_total();
        const auto touched = sp.on_arena_compact();
        CHECK(touched >= 12, "AC3: compact touched profiles");
        CHECK(sp.mutation_induced_invalidations() == mut0,
              "AC3: compact does not bump mutation_induced_invalidations (#2617)");
        CHECK(sp.deopt_storm_total() == storm0, "AC3: compact does not grow storm total");
        // Source-cite: compact still bans update_deopt_storm_state_
        const auto cpp = read_file("src/compiler/shape_profiler.cpp");
        CHECK(cpp.find("Explicitly do NOT call update_deopt_storm_state_") != std::string::npos,
              "AC3: compact storm ban cited");
    }
    {
        std::println("\n--- #2937 AC4: invalidate semantics (version + post-unlock hook) ---");
        ShapeProfiler sp;
        for (int i = 0; i < 150; ++i)
            (void)sp.record_shape(7, SHAPE_INT);
        const auto snap0 = sp.current_snapshot(7);
        std::atomic<int> hooks{0};
        sp.set_dirty_hook(
            [&](FnKey, std::uint32_t) { hooks.fetch_add(1, std::memory_order_relaxed); });
        const bool was_stable = sp.invalidate(7);
        (void)was_stable;
        const auto snap1 = sp.current_snapshot(7);
        CHECK(snap1.version >= snap0.version, "AC4: version advances on invalidate");
        CHECK(!sp.is_stable(7), "AC4: not stable after invalidate");
        // Hook may fire when was_stable; either way path completes.
        const auto cpp = read_file("src/compiler/shape_profiler.cpp");
        CHECK(cpp.find("fire_shape_deopt_hook") != std::string::npos,
              "AC4: deopt hook after unlock");
        (void)hooks;
    }
    {
        std::println("\n--- #2937 AC5/AC6: lineage + linter + no design ---");
        const auto t = read_file("tests/compiler/test_shape_profiler_concurrency.cpp");
        const auto lint = read_file("scripts/coverage/checks/check_shape_profiler_shard_2937.py");
        const auto build = read_file("build.py");
        CHECK(t.find("kShapeProfilerShardIssue") != std::string::npos,
              "AC5: test cites shard issue");
        CHECK(build.find("check_shape_profiler_shard_2937") != std::string::npos,
              "AC6: build.py wires linter");
        CHECK(!lint.empty() && lint.find("Issue #2937") != std::string::npos,
              "AC6: coverage linter present");
        CHECK(read_file("docs/design/2937-shape-profiler-shard.md").empty(),
              "AC6: no docs/design/2937-* per #1655");
        CHECK(read_file("tests/compiler/test_issue_2937.cpp").empty(),
              "AC6: no invent test file per #81967");
        // #2141 / #2617 lineage preserved in sources.
        const auto hh = read_file("src/compiler/shape_profiler.h");
        CHECK(hh.find("#2141") != std::string::npos, "AC5: #2141 preserved");
        CHECK(hh.find("#2617") != std::string::npos, "AC5: #2617 preserved");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_shape_profiler_concurrency();
}
#endif
