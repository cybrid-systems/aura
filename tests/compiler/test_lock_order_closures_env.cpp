// @category: unit
// @reason: Issue #1664 — lock order closures_mtx_ → env_frames_mtx_
// Issue #1664 (#1978 renamed): issue# moved from filename to header.
// must be consistent across probe_linear_ownership_* and
// scan_live_closures_for_linear_captures (no reverse-order deadlock).
//
//   AC1: scan_live_closures alone completes
//   AC2: probe at GC safepoint alone completes
//   AC3: probe on fiber steal alone completes
//   AC4: concurrent scan + GC probe + fiber steal (no hang)
//   AC5: concurrent with apply_closure (no hang)
//   AC6: stress 4 threads × 500 ops completes quickly
//
// Issue #4542 — global order closures shards → env shards → env_frames_mtx_.
// truncate_env_frames_to_checkpoint / compact_env_frames used to take the
// closures shards while holding the env shard barrier + env_frames_mtx_
// (env→closures), inverting against scan_live_closures (closures→env);
// TSan reported it 17/17 on the MutationBoundaryGuard dtor path.
//   AC7: single-thread scan → truncate → compact → scan (TSan lock graph
//        face: the pre-fix build reports lock-order-inversion here);
//        doomed closure still forced to bridge_epoch 0, compact still
//        remaps env_id
//   AC8: concurrent truncate+compact (+ Guard-dtor enforce) vs scan vs
//        closure registration — watchdog fails (instead of hanging) on deadlock
//   AC10: truncate leaves a doomed closure OOB, then compact — the compact
//        tail's refresh_stale_frames_after_steal used to re-enter compact
//        forever (stack overflow); must return
//   AC9: source cite — closures shards acquired before the env barrier in
//        truncate + compact; no closures acquisition under env locks

#include "test_harness.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::SymId;
using aura::compiler::Closure;
using aura::compiler::CompilerService;
using aura::compiler::EnvId;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

static void seed_moved_closure(Evaluator& ev) {
    auto env_id = ev.alloc_env_frame(NULL_ENV_ID);
    if (auto* fr = ev.resolve_env_frame_mut(env_id)) {
        fr->bindings_symid_.push_back({static_cast<SymId>(1), make_int(0)});
        fr->bindings_linear_ownership_state_.push_back(4); // Moved
        fr->version_ = ev.defuse_version_snapshot();
    }
    Closure cl;
    cl.env_id = env_id;
    (void)ev.register_active_closure(std::move(cl));
}

static void ac1_scan() {
    std::println("\n--- AC1: scan_live_closures ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    seed_moved_closure(ev);
    auto r = ev.scan_live_closures_for_linear_captures(true, true);
    CHECK(r.examined >= 1, "examined ≥1");
}

static void ac2_gc_probe() {
    std::println("\n--- AC2: probe at GC safepoint ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    seed_moved_closure(ev);
    ev.test_probe_linear_at_gc_safepoint();
    CHECK(true, "gc safepoint probe completed");
}

static void ac3_steal_probe() {
    std::println("\n--- AC3: probe on fiber steal ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    seed_moved_closure(ev);
    ev.test_probe_linear_on_fiber_steal();
    CHECK(true, "fiber steal probe completed");
}

static void ac4_concurrent_probes() {
    std::println("\n--- AC4: concurrent scan + probes ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 8; ++i)
        seed_moved_closure(ev);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> ops{0};
    std::vector<std::thread> thr;

    thr.emplace_back([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)ev.scan_live_closures_for_linear_captures(true, false);
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    });
    thr.emplace_back([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            ev.test_probe_linear_at_gc_safepoint();
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    });
    thr.emplace_back([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            ev.test_probe_linear_on_fiber_steal();
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const auto t0 = std::chrono::steady_clock::now();
    while (ops.load(std::memory_order_relaxed) < 150 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : thr)
        t.join();

    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(ops.load() >= 150, std::format("ops ≥150 (got {})", ops.load()));
    CHECK(elapsed < std::chrono::seconds(5), "completed under 5s (no deadlock hang)");
}

static void ac5_with_apply() {
    std::println("\n--- AC5: concurrent with apply_closure ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    seed_moved_closure(ev);
    CHECK(cs.eval("(set-code \"(define (f x) x)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> ops{0};
    std::thread probe([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            ev.test_probe_linear_at_gc_safepoint();
            (void)ev.scan_live_closures_for_linear_captures(false, false);
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread apply([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)cs.eval("(f 1)");
            ops.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const auto t0 = std::chrono::steady_clock::now();
    while (ops.load(std::memory_order_relaxed) < 100 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true, std::memory_order_relaxed);
    probe.join();
    apply.join();
    CHECK(ops.load() >= 100, "apply+probe ops ≥100");
    CHECK(true, "no hang with apply_closure");
}

static void ac6_stress() {
    std::println("\n--- AC6: 4-thread stress ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 4; ++i)
        seed_moved_closure(ev);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> total{0};
    auto worker = [&](int kind) {
        while (!stop.load(std::memory_order_relaxed)) {
            switch (kind % 3) {
                case 0:
                    (void)ev.scan_live_closures_for_linear_captures(true, true);
                    break;
                case 1:
                    ev.test_probe_linear_at_gc_safepoint();
                    break;
                default:
                    ev.test_probe_linear_on_fiber_steal();
                    break;
            }
            total.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> thr;
    for (int i = 0; i < 4; ++i)
        thr.emplace_back(worker, i);

    const auto t0 = std::chrono::steady_clock::now();
    while (total.load(std::memory_order_relaxed) < 500 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : thr)
        t.join();
    CHECK(total.load() >= 500, std::format("stress ≥500 (got {})", total.load()));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8), "stress under 8s");
}


// ── Issue #4542 ────────────────────────────────────────────────────────

static std::string read_src_4542(const char* rel) {
    for (const auto& base :
         {std::string(AURA_SOURCE_DIR) + "/", std::string(), std::string("../")}) {
        std::ifstream in(base + rel);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
    }
    return {};
}

static void ac7_truncate_compact_order() {
    std::println("\n--- AC7 (#4542): scan → truncate → compact → scan, one thread ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    seed_moved_closure(ev);
    // closures → env (scan) first, so a reverse edge later closes a cycle
    // in the TSan lock graph.
    (void)ev.scan_live_closures_for_linear_captures(true, false);

    for (int i = 0; i < 2; ++i)
        (void)ev.alloc_env_frame();
    const std::size_t base = ev.env_frames_size();
    ev.set_panic_safe_env_frames_size_for_test(base);
    const EnvId doomed_env = ev.alloc_env_frame();
    Closure cl;
    cl.name = "doomed4542";
    cl.env_id = doomed_env;
    cl.bridge_epoch = 42;
    const auto doomed_id = ev.register_active_closure(std::move(cl));
    CHECK(ev.truncate_env_frames_to_checkpoint() >= 1, "4542 AC7: truncate dropped ≥1");
    const auto snap = ev.find_active_closure(doomed_id);
    CHECK(snap.has_value(), "4542 AC7: doomed closure still registered");
    CHECK(snap && snap->bridge_epoch == 0,
          "4542 AC7: doomed bridge_epoch forced 0 (restamp kept under closures-first hold)");

    // compact: a referenced frame behind a reclaimable one must be remapped.
    const EnvId dead = ev.alloc_env_frame();
    const EnvId live = ev.alloc_env_frame();
    (void)dead;
    Closure keep;
    keep.name = "keep4542";
    keep.env_id = live;
    const auto keep_id = ev.register_active_closure(std::move(keep));
    (void)ev.compact_env_frames();
    const auto ksnap = ev.find_active_closure(keep_id);
    CHECK(ksnap.has_value(), "4542 AC7: compact keeps the referencing closure");
    CHECK(ksnap && (ksnap->env_id == NULL_ENV_ID || ksnap->env_id < ev.env_frames_size()),
          "4542 AC7: compact leaves env_id in range (remapped)");
    CHECK(ksnap && ksnap->env_id != NULL_ENV_ID && ev.resolve_env_frame(ksnap->env_id) != nullptr,
          "4542 AC7: remapped env_id resolves");
    (void)ev.scan_live_closures_for_linear_captures(true, false);
    (void)ev.enforce_linear_boundary_consistency(0, true);
    CHECK(true, "4542 AC7: sequence completed");
}

static void ac8_concurrent_truncate_compact_vs_scan() {
    std::println("\n--- AC8 (#4542): truncate/compact vs scan/enforce vs register ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 8; ++i)
        seed_moved_closure(ev);

    std::atomic<bool> stop{false};
    std::atomic<int> finished{0};
    std::atomic<std::uint64_t> structure_ops{0};
    std::atomic<std::uint64_t> scan_ops{0};
    std::atomic<std::uint64_t> reg_ops{0};

    // Structure thread: grows the arena past a checkpoint, truncates back,
    // and compacts — both take the closures shards + the env barrier.
    std::thread structure([&]() {
        int round = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const std::size_t base = ev.env_frames_size();
            ev.set_panic_safe_env_frames_size_for_test(base);
            for (int i = 0; i < 4; ++i)
                (void)ev.alloc_env_frame();
            (void)ev.truncate_env_frames_to_checkpoint();
            if ((++round % 4) == 0)
                (void)ev.compact_env_frames();
            structure_ops.fetch_add(1, std::memory_order_relaxed);
        }
        finished.fetch_add(1, std::memory_order_relaxed);
    });
    // Scan thread: the issue's closures → env path (scan + boundary enforce).
    std::thread scanner([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            // (enforce_linear_boundary_consistency → scan is the issue's
            // stack; the structure thread's free-standing truncate already
            // runs it via its MutationBoundaryGuard dtor. Calling it here
            // too would add a second writer to the linear audit ring —
            // a separate pre-existing race, not this lock-order face.)
            (void)ev.scan_live_closures_for_linear_captures(true, false);
            (void)ev.scan_live_closures_for_linear_captures(true, true);
            scan_ops.fetch_add(1, std::memory_order_relaxed);
        }
        finished.fetch_add(1, std::memory_order_relaxed);
    });
    // Registration thread (the only registering thread — next_id() is not
    // atomic): single-shard closures writes. Half carry an env_id past any
    // checkpoint so truncate's doomed-closure restamp has work to do.
    std::thread registrar([&]() {
        std::uint64_t n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            Closure c;
            c.env_id = (++n & 1) ? NULL_ENV_ID : static_cast<EnvId>(1u << 20);
            c.bridge_epoch = 7;
            (void)ev.register_active_closure(std::move(c));
            reg_ops.fetch_add(1, std::memory_order_relaxed);
        }
        finished.fetch_add(1, std::memory_order_relaxed);
    });

    using namespace std::chrono;
    const auto t0 = steady_clock::now();
    const auto run_for = milliseconds(1500);
    while (steady_clock::now() - t0 < run_for)
        std::this_thread::sleep_for(milliseconds(5));
    stop.store(true, std::memory_order_relaxed);
    // Watchdog: a lock-order deadlock parks two threads forever. Fail loudly
    // instead of hanging the runner.
    const auto deadline = steady_clock::now() + seconds(60);
    while (finished.load(std::memory_order_relaxed) < 3 && steady_clock::now() < deadline)
        std::this_thread::sleep_for(milliseconds(10));
    if (finished.load(std::memory_order_relaxed) < 3) {
        std::println("FAIL: 4542 AC8: workers wedged (structure={} scan={} reg={}) — "
                     "closures/env lock-order deadlock",
                     structure_ops.load(), scan_ops.load(), reg_ops.load());
        std::fflush(stdout);
        std::_Exit(1);
    }
    structure.join();
    scanner.join();
    registrar.join();
    CHECK(structure_ops.load() > 0,
          std::format("4542 AC8: truncate/compact rounds ran (got {})", structure_ops.load()));
    CHECK(scan_ops.load() > 0, std::format("4542 AC8: scans ran (got {})", scan_ops.load()));
    CHECK(reg_ops.load() > 0, std::format("4542 AC8: registrations ran (got {})", reg_ops.load()));
    CHECK(true, "4542 AC8: all workers drained (no deadlock)");
}

static void ac10_compact_after_truncate_no_recursion() {
    std::println("\n--- AC10 (#4542): truncate → compact with an OOB doomed closure ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    (void)ev.alloc_env_frame();
    const std::size_t base = ev.env_frames_size();
    ev.set_panic_safe_env_frames_size_for_test(base);
    for (int i = 0; i < 3; ++i) {
        Closure c;
        c.env_id = ev.alloc_env_frame();
        c.bridge_epoch = 9;
        (void)ev.register_active_closure(std::move(c));
    }
    CHECK(ev.truncate_env_frames_to_checkpoint() >= 3, "4542 AC10: truncate dropped the tail");
    (void)ev.compact_env_frames();
    (void)ev.compact_env_frames();
    CHECK(true, "4542 AC10: compact returned (no compact ⇄ refresh recursion)");
}

static void ac9_source_cite() {
    std::println("\n--- AC9 (#4542): closures-first acquisition in truncate + compact ---");
    const auto src = read_src_4542("src/compiler/evaluator_env.cpp");
    CHECK(!src.empty(), "4542 AC9: read evaluator_env.cpp");
    const auto cl_take =
        std::string("std::unique_lock<std::shared_mutex>(closures_shards_[cl_i].mu);");
    const auto ef_take = std::string(
        "ef_barrier[ef_i] = std::unique_lock<std::shared_mutex>(env_frame_shards_[ef_i].mu);");
    for (const char* fn : {"std::size_t Evaluator::truncate_env_frames_to_checkpoint() {",
                           "std::size_t Evaluator::compact_env_frames() {"}) {
        const auto pos = src.find(fn);
        CHECK(pos != std::string::npos, std::format("4542 AC9: located {}", fn));
        if (pos == std::string::npos)
            continue;
        auto end = src.find("\n}\n", pos);
        const auto body = src.substr(pos, end == std::string::npos ? 30000 : end - pos);
        const auto first_cl = body.find(cl_take);
        const auto barrier = body.find(ef_take);
        CHECK(first_cl != std::string::npos && barrier != std::string::npos && first_cl < barrier,
              std::format("4542 AC9: closures shards precede the env barrier in {}", fn));
        // Exactly one closures acquisition before the barrier release; any
        // later one must come after every env lock is dropped.
        const auto second_cl = body.find(cl_take, first_cl + 1);
        if (second_cl != std::string::npos) {
            const auto env_unlock = body.rfind("env_lock.unlock();", second_cl);
            CHECK(env_unlock != std::string::npos && env_unlock > barrier,
                  std::format("4542 AC9: later closures take follows env unlock in {}", fn));
        } else {
            CHECK(true, std::format("4542 AC9: single closures hold in {}", fn));
        }
        CHECK(body.find("#4542") != std::string::npos,
              std::format("4542 AC9: cites #4542 in {}", fn));
    }
    const auto ixx = read_src_4542("src/compiler/evaluator.ixx");
    CHECK(ixx.find("closures_shards_[*].mu  →  env_frame_shards_[*].mu  →  env_frames_mtx_") !=
              std::string::npos,
          "4542 AC9: global order documented in evaluator.ixx");
}

// Issue #4580 AC11: concurrent closure registration must draw unique
// ClosureIds. register_active_closure (and the flat-eval sites) draw
// next_id() BEFORE taking the closures shard lock — a plain counter handed
// two threads the same id, aliasing shard entries and breaking
// gc_safe_closure_id_ bookkeeping.
static void ac11_next_id_unique_concurrent() {
    std::println("\n--- AC11 (#4580): concurrent ClosureId uniqueness ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    constexpr int kThreads = 4;
    constexpr int kPerThread = 400;
    std::vector<std::uint64_t> ids(static_cast<std::size_t>(kThreads) * kPerThread, 0);
    std::vector<std::thread> thr;
    for (int t = 0; t < kThreads; ++t) {
        thr.emplace_back([&, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                Closure cl;
                cl.env_id = NULL_ENV_ID;
                const auto id = ev.register_active_closure(std::move(cl));
                ids[static_cast<std::size_t>(t) * kPerThread + static_cast<std::size_t>(i)] = id;
            }
        });
    }
    for (auto& t : thr)
        t.join();
    std::sort(ids.begin(), ids.end());
    CHECK(ids.front() != 0, "4580 AC11: ids nonzero");
    const auto dup = std::adjacent_find(ids.begin(), ids.end());
    CHECK(dup == ids.end(), "4580 AC11: all ClosureIds unique under concurrent registration");
}

} // namespace

int main() {
    std::println("=== Issue #1664: closures→env lock order (no reverse deadlock) ===");
    ac1_scan();
    ac2_gc_probe();
    ac3_steal_probe();
    ac4_concurrent_probes();
    ac5_with_apply();
    ac6_stress();
    ac7_truncate_compact_order();
    ac8_concurrent_truncate_compact_vs_scan();
    ac10_compact_after_truncate_no_recursion();
    ac9_source_cite();
    ac11_next_id_unique_concurrent();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
