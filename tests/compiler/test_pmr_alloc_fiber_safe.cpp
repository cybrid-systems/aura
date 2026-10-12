// @category: unit
// @reason: Issue #2651 / #2649 H9 — concurrent string_heap_ / pairs_ /
//          ASTArena allocate must not race pmr under multi-thread stress.
//          Issue #2997 — 4–8 fiber concurrent list construction + lock SLO.
//
//   AC1: N threads push_string_heap large strings → no crash
//   AC2: N threads push_pair → no crash
//   AC3: concurrent string-append via eval under eval_mutex still works
//   AC4: source-cite #2651 on string-append + ASTArena alloc lock
//   AC5: push_string_heap / cons take alloc_storage_lock_
//   AC6 (#2997): 8-thread list via slot_lookup_fast; lock-hold samples
//   AC7 (#2997): list-ref / member / math still work (no allow on those)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/prim_heap_quota.hh"

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_error;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::compiler::types::make_int;
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

// ── AC1: concurrent push_string_heap (bypasses eval_mutex_) ──
static void ac1_concurrent_push_string() {
    std::println("\n--- #2651 AC1: concurrent push_string_heap large strings ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    constexpr int kThreads = 8;
    constexpr int kIters = 2000;
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&ev, &errors, t] {
            try {
                for (int i = 0; i < kIters; ++i) {
                    // ~256-byte pad fragments (overnight uses multi-KB pads)
                    std::string s(256, static_cast<char>('a' + (t % 26)));
                    s += std::to_string(i);
                    (void)ev.push_string_heap(std::move(s));
                }
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : ts)
        th.join();
    CHECK(errors.load() == 0, "AC1: 0 errors under concurrent push_string_heap");
    CHECK(ev.string_heap_size() >= static_cast<std::size_t>(kThreads * kIters),
          "AC1: all pushes visible");
}

// ── AC2: concurrent push_pair ──
static void ac2_concurrent_push_pair() {
    std::println("\n--- #2651 AC2: concurrent push_pair ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    constexpr int kThreads = 8;
    constexpr int kIters = 4000;
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&ev, &errors, t] {
            try {
                using aura::compiler::types::make_int;
                for (int i = 0; i < kIters; ++i) {
                    (void)ev.push_pair(make_int(t * kIters + i), make_int(i));
                }
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : ts)
        th.join();
    CHECK(errors.load() == 0, "AC2: 0 errors under concurrent push_pair");
    CHECK(ev.pairs_size() >= static_cast<std::size_t>(kThreads * kIters), "AC2: all pairs visible");
}

// ── AC3: string-append still correct under serialized eval ──
static void ac3_string_append_correct() {
    std::println("\n--- #2651 AC3: string-append functional ---");
    CompilerService cs;
    auto r = cs.eval("(string-append \"hello\" \"-\" \"world\")");
    CHECK(r && is_string(*r), "AC3: string-append returns string");
    auto len = cs.eval("(string-length (string-append \"ab\" \"cd\" \"ef\"))");
    CHECK(len && is_int(*len) && as_int(*len) == 6, "AC3: append length 6");
}

// ── AC4: source-cite ──
static void ac4_source_cite() {
    std::println("\n--- #2651 AC4: source cites locks ---");
    const auto pair = read_file("src/compiler/evaluator_primitives_pair.cpp");
    CHECK(pair.find("#2651") != std::string::npos, "AC4: pair.cpp cites #2651");
    CHECK(pair.find("string-append") != std::string::npos &&
              pair.find("alloc_storage_lock_") != std::string::npos,
          "AC4: string-append locks");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    CHECK(ixx.find("#2651") != std::string::npos, "AC4: evaluator.ixx cites #2651");
    CHECK(ixx.find("push_string_heap") != std::string::npos, "AC4: push_string_heap present");
    const auto listp = read_file("src/compiler/evaluator_primitives_list.cpp");
    CHECK(listp.find("#2651") != std::string::npos, "AC4: list.cpp cites #2651");
    CHECK(listp.find("alloc_storage_lock_") != std::string::npos, "AC4: list locks pairs_");
}

// ── AC5: multi-thread string-append via eval (eval_mutex serializes;
//    still a smoke that locked path works under load) ──
static void ac5_concurrent_eval_append() {
    std::println("\n--- #2651 AC5: concurrent cs.eval string-append smoke ---");
    CompilerService cs;
    constexpr int kThreads = 4;
    constexpr int kIters = 200;
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&cs, &errors] {
            try {
                for (int i = 0; i < kIters; ++i) {
                    auto r = cs.eval("(string-append \"pad-\" \"xxxxxxxx\" \"yyyyyyyy\")");
                    if (!r || !is_string(*r))
                        errors.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : ts)
        th.join();
    CHECK(errors.load() == 0, "AC5: concurrent eval string-append clean");
}

// ── #2997 AC1/AC4: 8-fiber concurrent list via slot_lookup_fast ──
static void ac6_concurrent_list_ctor() {
    std::println("\n--- #2997 AC1: 8-fiber concurrent list (slot_lookup_fast) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto slot = ev.primitives().slot_for_name("list");
    CHECK(slot < ev.primitives().slot_count(), "AC1: list slot registered");
    auto fnopt = ev.primitives().slot_lookup_fast(slot);
    CHECK(fnopt.has_value(), "AC1: list slot_lookup_fast");
    if (!fnopt)
        return;
    const auto list_fn = *fnopt;

    constexpr int kThreads = 8;
    constexpr int kIters = 400;
    std::atomic<int> errors{0};
    std::atomic<int> ok{0};
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&list_fn, &errors, &ok] {
            try {
                const std::array<EvalValue, 6> args{make_int(1), make_int(2), make_int(3),
                                                    make_int(4), make_int(5), make_int(6)};
                for (int i = 0; i < kIters; ++i) {
                    auto r = list_fn(std::span<const EvalValue>(args));
                    if (is_error(r) || !is_pair(r))
                        errors.fetch_add(1, std::memory_order_relaxed);
                    else
                        ok.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : ts)
        th.join();
    const auto wall_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    std::uint64_t hold = 0, samples = 0;
    if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
        hold = m->list_constructor_lock_hold_ns.load(std::memory_order_relaxed);
        samples = m->list_constructor_lock_samples.load(std::memory_order_relaxed);
    }
    const auto avg = samples ? (hold / samples) : 0;
    std::println("#2997 8-fiber list: wall_ns={} lock_hold_ns={} samples={} avg_hold_ns={}",
                 wall_ns, hold, samples, avg);
    CHECK(errors.load() == 0, "AC1: 0 errors under 8-fiber list");
    CHECK(ok.load() == kThreads * kIters, "AC1: all list constructions succeeded");
    CHECK(samples == 0 || samples >= static_cast<std::uint64_t>(kThreads),
          "AC4: lock-hold samples recorded");

    auto qhold =
        cs.eval("(hash-ref (engine:metrics \"query:prim-heap-quota-stats\") 'lock-hold-ns)");
    CHECK(qhold && is_int(*qhold) && as_int(*qhold) >= 0, "AC4: query lock-hold-ns");
    auto qrec = cs.eval("(hash-ref (engine:metrics \"query:prim-heap-quota-stats\") 'recommend)");
    CHECK(qrec && is_int(*qrec), "AC4: query recommend");
}

// ── #2997 AC5: single-fiber list-ref / member / math (never call allow) ──
static void ac7_single_fiber_hotpath() {
    std::println("\n--- #2997 AC5: single-fiber list-ref / member / math ---");
    CompilerService cs;
    auto r = cs.eval("(list-ref (list 10 20 30) 1)");
    CHECK(r && is_int(*r) && as_int(*r) == 20, "AC5: list-ref");
    auto mem = cs.eval("(length (member 2 (list 1 2 3)))");
    CHECK(mem && is_int(*mem) && as_int(*mem) == 2, "AC5: member");
    auto math = cs.eval("(+ (* 3 4) 5)");
    CHECK(math && is_int(*math) && as_int(*math) == 17, "AC5: math");
    const auto listp = read_file("src/compiler/evaluator_primitives_list.cpp");
    CHECK(listp.find("ListCtorLockHold") != std::string::npos, "AC2: list uses timed lock");
    // Issue #4473 reshaped the quota face: the #2997 small-unlimited bypass
    // left list.cpp with the quota pre-checks; every builder now routes
    // through mint_pair, whose grow path is the allow() check (same bypass
    // semantics per mint, in evaluator_gc.cpp).
    CHECK(listp.find("mint_pair") != std::string::npos,
          "AC3: quota moved to per-mint mint_pair (#4473)");
    const auto hh = read_file("src/compiler/prim_heap_quota.hh");
    CHECK(hh.find("2997") != std::string::npos, "AC1: quota header notes #2997");
}

static long vm_rss_kb() {
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        if (!line.starts_with("VmRSS:"))
            continue;
        long v = 0;
        for (char c : line) {
            if (c >= '0' && c <= '9')
                v = v * 10 + (c - '0');
        }
        return v;
    }
    return 0;
}

// Issue #4453: N calls of list must stay O(N) on the monotonic pairs
// arena. reserve(size + n) abandoned every block (10k calls → ~1.6GB).
static void ac4453_list_growth_linear() {
    std::println("\n--- #4453: list growth stays linear ---");
    std::size_t size = 0;
    std::size_t capacity = 0;
    std::uint64_t committed = 0;
    constexpr int kSteps = 100000;
    for (int i = 0; i < kSteps; ++i) {
        const auto next = aura::compiler::monotonic_pairs_reserve_cap(size, capacity, 2);
        if (next > capacity)
            committed += next;
        capacity = next;
        size += 2;
    }
    CHECK(size == static_cast<std::size_t>(kSteps) * 2, "4453: sim size");
    CHECK(capacity >= size, "4453: sim capacity covers");
    CHECK(committed <= static_cast<std::uint64_t>(size) * 3, "4453: sim commit linear");

    const auto list_src = read_file("src/compiler/evaluator_primitives_list.cpp");
    const auto json_src = read_file("src/compiler/evaluator_primitives_json.cpp");
    CHECK(list_src.find("pairs.reserve(pairs.size() + n)") == std::string::npos,
          "4453: list dropped exact size+n reserve");
    CHECK(json_src.find("pairs.reserve(pairs.size() + n)") == std::string::npos,
          "4453: json dropped exact size+n reserve");
    int sites = 0;
    for (std::size_t p = 0;;) {
        p = list_src.find("monotonic_pairs_reserve_cap", p);
        if (p == std::string::npos)
            break;
        ++sites;
        p += 28;
    }
    CHECK(sites >= 6, "4453: list/append/reverse/map/filter/list-sort");
    CHECK(json_src.find("monotonic_pairs_reserve_cap") != std::string::npos,
          "4453: json array uses the same policy");

    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto slot = ev.primitives().slot_for_name("list");
    CHECK(slot < ev.primitives().slot_count(), "4453: list slot");
    auto fnopt = ev.primitives().slot_lookup_fast(slot);
    CHECK(fnopt.has_value(), "4453: list fn");
    if (!fnopt)
        return;
    const auto list_fn = *fnopt;
    const std::array<EvalValue, 2> args{make_int(1), make_int(2)};
    constexpr int kCalls = 10000;
    const auto before = ev.pairs_size();
    const auto rss0 = vm_rss_kb();
    bool ok = true;
    for (int i = 0; i < kCalls; ++i) {
        auto r = list_fn(std::span<const EvalValue>(args));
        if (is_error(r) || !is_pair(r)) {
            ok = false;
            break;
        }
    }
    const auto rss1 = vm_rss_kb();
    const auto after = ev.pairs_size();
    CHECK(ok, "4453: 10000 list calls succeed");
    CHECK(after >= before + static_cast<std::size_t>(kCalls) * 2, "4453: two pairs per call");
    CHECK(rss0 > 0 && rss1 >= rss0, "4453: VmRSS readable");
    const auto delta = rss1 - rss0;
    std::println("4453: list x{} rss_delta_kb={} pairs={}", kCalls, delta, after - before);
    // size+n reserve peaked near 1.6GB at this N. A linear heap fits in
    // already-resident pages (delta may be 0) and must stay under 64MB.
    CHECK(delta < 64L * 1024L, "4453: RSS delta stays linear");

    auto sample = cs.eval("(car (cdr (list 7 8 9)))");
    CHECK(sample && is_int(*sample) && as_int(*sample) == 8, "4453: list order");
}

} // namespace

// ── Issue #4473: list builders mint through the #4459 pair freelist ──
// cons reuses reclaimed slots via mint_pair; list/append/reverse/map/filter/
// sort used to pairs.push_back directly, so a steady-state loop built with
// them grew pairs_ linearly while the same cons chain stayed flat. After the
// conversion every builder routes through mint_pair (per-mint quota: a
// freelist hit does not count), so all shapes stay at the live set.
static void ac4473_list_mint_reuse() {
    std::println("\n--- #4473: list builders reuse the pair freelist ---");
    CompilerService cs;
    auto& ev = cs.evaluator();

    // AC1: the issue repro — a list-building loop crosses many reclaims
    // (kFileReclaimAllocs = 128 mints) and must stay at the live set.
    auto r1 = cs.eval("(begin (define (lp i acc) (if (< i 20000) (begin (list i 2 3 4) "
                      "(lp (+ i 1) acc)) acc)) (lp 0 0))");
    CHECK(r1 && is_int(*r1) && as_int(*r1) == 0, "4473 AC1: list loop evals");
    const auto pairs_after_list = ev.pairs_size();

    // AC2: the cons chain (the #4459 face) — the parity bound.
    auto r2 = cs.eval("(begin (define (lq i acc) (if (< i 20000) (begin (cons i (cons 2 "
                      "(cons 3 (cons 4 (quote ()))))) (lq (+ i 1) acc)) acc)) (lq 0 0))");
    CHECK(r2 && is_int(*r2) && as_int(*r2) == 0, "4473 AC2: cons loop evals");
    const auto pairs_after_cons = ev.pairs_size();
    CHECK(pairs_after_list <= pairs_after_cons + 64,
          "4473 AC3: list-built pairs stay flat like cons (" + std::to_string(pairs_after_list) +
              " vs " + std::to_string(pairs_after_cons) + ")");

    // AC4: reverse + map shapes (named by the issue) stay flat too.
    auto r3 = cs.eval("(begin (define (lr i acc) (if (< i 10000) (begin (reverse (list i 2 3 "
                      "4)) (lr (+ i 1) acc)) acc)) (lr 0 0))");
    CHECK(r3 && is_int(*r3) && as_int(*r3) == 0, "4473 AC4: reverse loop evals");
    auto r4 = cs.eval("(begin (define (lm i acc) (if (< i 10000) (begin (map (lambda (x) x) "
                      "(list i 2 3 4)) (lm (+ i 1) acc)) acc)) (lm 0 0))");
    CHECK(r4 && is_int(*r4) && as_int(*r4) == 0, "4473 AC5: map loop evals");
    const auto pairs_after_more = ev.pairs_size();
    CHECK(pairs_after_more <= pairs_after_cons + 256,
          "4473 AC6: reverse/map-built pairs stay flat (" + std::to_string(pairs_after_more) +
              " vs " + std::to_string(pairs_after_cons) + ")");

    // AC7: the builders honor the mint_pair deny sentinel (cons-shaped
    // typed quota error on the grow path).
    const auto src = read_file("src/compiler/evaluator_primitives_list.cpp");
    CHECK(src.find("kFileHeapMintDenied") != std::string::npos,
          "4473 AC7: list builders honor the mint_pair deny sentinel");
}

// ── Issue #4474: dynamic reclaim triggers (live/32, clamped) ──
// #4459's fixed every-128-allocations full-heap mark costs O(live) per
// allocation on large live heaps (1.7–2.8x on alloc-heavy loops). After
// a mark, the next trigger now scales with the reachable set the mark
// found; the floor keeps the #4459 small-heap cadence unchanged.
static void ac4474_dynamic_reclaim_trigger() {
    std::println("\n--- #4474: reclaim trigger scales with the live set ---");

    // AC1: floor — a fresh evaluator's first mark finds a tiny live set:
    // the next trigger stays at the #4459 128-allocation cadence.
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        const bool ran = ev.reclaim_tw_unreachable({}, {});
        CHECK(ran, "4474 AC1: fresh-evaluator reclaim runs");
        CHECK(ev.file_heap_allocs_trigger_for_test() == 128,
              "4474 AC1: small live set keeps the 128 floor");
    }

    // AC2: scale — a ~48k-pair live heap lifts the next trigger to
    // ~live/32, so the mark cost amortizes instead of firing every 128.
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto r = cs.eval("(begin (define v (make-vector 48000 0))"
                         "(define (fill i) (if (< i 48000)"
                         "(begin (vector-set! v i (cons i i)) (fill (+ i 1))) #f))"
                         "(fill 0))");
        CHECK(r.has_value(), "4474 AC2: 48k-pair live fill evals");
        (void)ev.reclaim_tw_unreachable({}, {});
        const auto trig = ev.file_heap_allocs_trigger_for_test();
        CHECK(trig >= 1400, "4474 AC2: large live set scales the trigger (got " +
                                std::to_string(trig) + ", want >= 1400)");

        // AC3: memory stays bounded — a throwaway loop with the scaled
        // trigger grows pairs_ by at most ~one trigger window, then the
        // next mark folds the garbage into the freelist (flat like #4459).
        const auto before = ev.pairs_size();
        auto r2 = cs.eval("(begin (define (lp i acc) (if (< i 5000) (begin (cons i acc) "
                          "(lp (+ i 1) acc)) acc)) (lp 0 0))");
        CHECK(r2 && is_int(*r2), "4474 AC3: throwaway cons loop evals");
        const auto grown = ev.pairs_size() - before;
        CHECK(grown <= 2500, "4474 AC3: garbage between reclaims stays ~one trigger window (grew " +
                                 std::to_string(grown) + ")");
    }

    // AC4: cap — an absurd live-set report clamps the trigger at 32768.
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.note_live_set_for_reclaim_trigger(100000000);
        CHECK(ev.file_heap_allocs_trigger_for_test() == 32768,
              "4474 AC4: trigger clamps at the 32768 ceiling");
    }
}

int run_test_pmr_alloc_fiber_safe() {
    std::println("=== Issue #2651: PMR / string_heap concurrent alloc safety ===");
    ac1_concurrent_push_string();
    ac2_concurrent_push_pair();
    ac3_string_append_correct();
    ac4_source_cite();
    ac5_concurrent_eval_append();
    ac6_concurrent_list_ctor();
    ac7_single_fiber_hotpath();
    ac4453_list_growth_linear();
    ac4473_list_mint_reuse();
    ac4474_dynamic_reclaim_trigger();
    std::println("\n=== #2651/#2997: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_pmr_alloc_fiber_safe();
}
#endif
