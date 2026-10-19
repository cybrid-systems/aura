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
import aura.core.ast;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_float;
using aura::compiler::types::as_int;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_float;
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

    // AC4: cap — a huge live-set report scales the window past the old
    // 32768 ceiling (live/32 + 128, which max(32768, live/4) never
    // reaches), and the #4525 burst-doubling escalation is bounded by
    // the heap-proportional ceiling (live/4 here) instead of the old
    // fixed cap. Third reopen: the fixed cap froze the window above
    // ~1M live objects and per-alloc cost regrew linearly with the heap.
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.note_live_set_for_reclaim_trigger(100000000, /*mints_since_mark=*/0);
        CHECK(ev.file_heap_allocs_trigger_for_test() == 100000000 / 32 + 128,
              "4474 AC4: huge live set scales past the old 32768 ceiling (got " +
                  std::to_string(ev.file_heap_allocs_trigger_for_test()) + ", want " +
                  std::to_string(100000000 / 32 + 128) + ")");
    }
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Burst shape: 1M live, 4B mints — live*2 < mints doubles the
        // window every call until the live/4 proportional ceiling holds it.
        for (int i = 0; i < 24; ++i)
            ev.note_live_set_for_reclaim_trigger(1000000, 4000000000ULL);
        CHECK(ev.file_heap_allocs_trigger_for_test() == 250000,
              "4474 AC4: burst escalation clamps at the live/4 proportional ceiling (got " +
                  std::to_string(ev.file_heap_allocs_trigger_for_test()) + ", want 250000)");
    }
}

// ── Issue #4474 (third reopen): the window must keep scaling on ──
// ── multi-million-object live heaps ──
// kReclaimTriggerCeiling=32768 pinned the window above ~1M live
// objects, so each O(live) full-heap mark amortized over at most 32768
// allocations — the per-allocation cost grew linearly with the live
// heap again (reopen: ~3x alloc overhead at 3M live cells vs 300k;
// gdb 25/30 samples in maybe_reclaim_tw_unreachable; aura-memory
// write p50 0.82→1.57ms at a 20k-item store). The ceiling is now
// heap-proportional (max(32768, live/4)): live/32 scaling never hits
// it, and it only bounds the #4525 burst-doubling escalation.
static void ac4474r3_large_heap_window() {
    std::println("\n--- #4474 r3: window scales on multi-million live heaps ---");
    using clock = std::chrono::steady_clock;

    // Reopen repro shape: hold N live pairs in a global, force one mark
    // so the window reflects the live set, then time reps calls of a
    // 100-cons loop against an empty loop. The alloc-minus-empty
    // difference is the per-allocation reclaim amortization; both loops
    // run back to back in this process, so machine load cancels in the
    // big-vs-small ratio.
    struct Sample {
        std::uint32_t trigger;
        double alloc_us;
        double empty_us;
    };
    constexpr int kReps = 4000;
    const auto measure = [&](int n) -> Sample {
        CompilerService cs;
        auto& ev = cs.evaluator();
        const std::string prog = "(begin (define big (let loop ((i 0) (acc (list)))"
                                 "(if (< i " +
                                 std::to_string(n) + ") (loop (+ i 1) (cons i acc)) acc)))" +
                                 "(define (alloc) (let loop ((i 0) (acc (list)))"
                                 "(if (< i 100) (loop (+ i 1) (cons i acc)) acc)))" +
                                 "(define (empty) (let loop ((i 0) (acc (list)))"
                                 "(if (< i 100) (loop (+ i 1) acc) acc)))" +
                                 "(define (drive f n) (let loop ((i 0))"
                                 "(if (< i n) (begin (f) (loop (+ i 1))) #f)))" +
                                 "(length big))";
        auto built = cs.eval(prog);
        CHECK(built && is_int(*built) && as_int(*built) == n,
              "4474 r3: live list builds (n=" + std::to_string(n) + ")");
        (void)ev.reclaim_tw_unreachable({}, {});
        const auto trigger = ev.file_heap_allocs_trigger_for_test();
        const auto t0 = clock::now();
        auto re = cs.eval("(drive empty " + std::to_string(kReps) + ")");
        const auto t1 = clock::now();
        auto ra = cs.eval("(drive alloc " + std::to_string(kReps) + ")");
        const auto t2 = clock::now();
        CHECK(static_cast<bool>(re) && static_cast<bool>(ra),
              "4474 r3: drive loops eval (n=" + std::to_string(n) + ")");
        const auto ms = [](clock::duration d) {
            return std::chrono::duration<double, std::milli>(d).count();
        };
        return Sample{trigger, ms(t2 - t1) * 1000.0 / kReps, ms(t1 - t0) * 1000.0 / kReps};
    };

    const Sample mid = measure(300000);
    const Sample big = measure(3000000);

    // AC1: the window itself — ~300k live (flat region before and after
    // the fix) and ~3M live both sit at live/32 + 128; the 3M window
    // grows past the old 32768 ceiling instead of freezing there.
    CHECK(mid.trigger >= 300000 / 32 + 128 && mid.trigger <= 9600,
          "4474 r3 AC1: ~300k live keeps the live/32 window (got " + std::to_string(mid.trigger) +
              ", want >= " + std::to_string(300000 / 32 + 128) + ")");
    CHECK(big.trigger >= 93000 && big.trigger <= 95500,
          "4474 r3 AC1: 3M live grows the window past the old 32768 ceiling (got " +
              std::to_string(big.trigger) + ")");

    // AC2: allocation amortization — the reopen measured alloc overhead
    // (alloc minus empty) roughly tripling at 3M live vs the 300k flat
    // region; with the proportional window the amortized mark cost per
    // allocation is the same constant (live/window ≈ 32) at both sizes.
    // Generous 2x + 3µs bound — pre-fix ratio is ~2.9x here.
    const double ovh_mid = mid.alloc_us - mid.empty_us;
    const double ovh_big = big.alloc_us - big.empty_us;
    CHECK(ovh_mid > 0.0 && ovh_big > 0.0, "4474 r3 AC2: overheads measurable (mid " +
                                              std::to_string(ovh_mid) + "µs, big " +
                                              std::to_string(ovh_big) + "µs per 100 conses)");
    CHECK(ovh_big <= 2.0 * ovh_mid + 3.0,
          "4474 r3 AC2: 3M-live alloc amortization within 2x of 300k (" + std::to_string(ovh_big) +
              "µs vs " + std::to_string(ovh_mid) + "µs per 100 conses)");
    std::println(
        "    4474 r3: window mid={} big={}  alloc/empty µs mid={:.1f}/{:.1f} big={:.1f}/{:.1f}",
        mid.trigger, big.trigger, mid.alloc_us, mid.empty_us, big.alloc_us, big.empty_us);
}

// ── Issue #4480: map's accumulator must survive reclaims triggered by the
// mapped procedure. #4459's tree-walker safepoint marks envs and pinned
// temporaries only; map accumulated row results into a raw host vector,
// so a reclaim inside a nested user-procedure call freelisted the already
// computed row pairs and the later row mints plus the outer-chain build
// recycled those slots — the returned list aliased live rows (early
// elements lost their heads and showed later elements; the 3-element
// issue repro stopped firing after #4474 lifted the trigger, so AC2
// forces the reclaim window with 200 rows × 2 mints > the 128 floor).
// The map lambda now holds Evaluator::TwVecPin roots over cars/mapped
// for the whole apply + construction.
static void ac4480_map_accumulator_pinned() {
    std::println("\n--- #4480: map results survive nested-call reclaims ---");
    CompilerService cs;

    // AC1: the issue repro — row calls f before (list x 2).
    auto r1 = cs.eval("(begin (define (f) 1)"
                      "(define (row x) (f) (list x 2))"
                      "(equal? (map row (list 1 2 3))"
                      "(quote ((1 2) (2 2) (3 2)))))");
    CHECK(r1 && is_bool(*r1) && as_bool(*r1),
          "4480 AC1: issue repro returns one fresh correct row per element");

    // AC2: force the #4459 reclaim mid-apply. 200 rows × 2 mints crosses
    // the 128-allocation floor between marks, so nested (f) calls hit the
    // safepoint while earlier row pairs are live only in map's
    // accumulator (the pre-fix corruption face).
    auto r2 = cs.eval("(begin (define (build i acc) (if (< i 0) acc (build (- i 1) (cons i acc))))"
                      "(define (ok res i) (if (null? res) #t"
                      "(if (and (= (car (car res)) i) (= (car (cdr (car res))) 2))"
                      "(ok (cdr res) (+ i 1)) #f)))"
                      "(ok (map (lambda (x) (f) (list x 2)) (build 199 (quote ()))) 0))");
    CHECK(r2 && is_bool(*r2) && as_bool(*r2),
          "4480 AC2: 200-row map under mid-apply reclaim stays fresh");

    // AC3: the hand-written recursive map (rooted via env frames) keeps
    // working with the same row — the pin must not change its face.
    auto r3 = cs.eval("(begin (define (mymap fn lst)"
                      "(if (null? lst) (quote ())"
                      "(cons (fn (car lst)) (mymap fn (cdr lst)))))"
                      "(equal? (mymap row (list 1 2 3))"
                      "(quote ((1 2) (2 2) (3 2)))))");
    CHECK(r3 && is_bool(*r3) && as_bool(*r3), "4480 AC3: hand-written recursive map keeps working");

    // AC4: plain map with no callee never reclaims mid-apply.
    auto r4 = cs.eval("(equal? (map (lambda (x) (list x 2)) (list 1 2 3))"
                      "(quote ((1 2) (2 2) (3 2))))");
    CHECK(r4 && is_bool(*r4) && as_bool(*r4), "4480 AC4: plain map without a callee keeps working");

    // AC5: the fix's anchor — map holds the accumulator root.
    const auto src = read_file("src/compiler/evaluator_primitives_list.cpp");
    CHECK(src.find("TwVecPin pin_mapped") != std::string::npos,
          "4480 AC5: map holds a TwVecPin root over its accumulator");
}

// ── Issue #4492: nested map (mapped procedure allocates) and filter with an
// allocating pred must survive reclaims forced by garbage between builds.
static void ac4492_nested_map_filter_pinned() {
    std::println("\n--- #4492: nested map / allocating filter survive reclaims ---");
    CompilerService cs;
    auto r1 = cs.eval(
        "(begin"
        "(define (index-of x xs) (let lp ((xs xs) (i 0)) (cond ((null? xs) -1)"
        "((eq? (car xs) x) i) (else (lp (cdr xs) (+ i 1))))))"
        "(define names (list 'a 'b 'c))"
        "(define (build) (map (lambda (a) (map (lambda (b) (index-of b names)) names)) names))"
        "(define (garbage n) (let lp ((i 0) (acc (list))) (if (< i n)"
        "(lp (+ i 1) (cons i (if (> (length acc) 20) (list) acc))) #t)))"
        "(define want (quote ((0 1 2) (0 1 2) (0 1 2))))"
        "(define v1 (build)) (garbage 2000)"
        "(define v2 (build)) (garbage 2000)"
        "(define v3 (build)) (garbage 2000)"
        "(and (equal? v1 want) (equal? v2 want) (equal? v3 want)))");
    CHECK(r1 && is_bool(*r1) && as_bool(*r1),
          "4492 AC1: nested-map results stay intact across forced reclaims");

    for (const char* d :
         {"(define (seq i acc) (if (< i 0) acc (seq (- i 1) (cons i acc))))",
          "(define (fb) (filter (lambda (p) (list p p p) (= (remainder (car p) 2) 0))"
          " (map (lambda (x) (list x 1)) (seq 199 (quote ())))))",
          "(define (ok res i) (if (null? res) (= i 200)"
          " (if (and (= (car (car res)) i) (= (car (cdr (car res))) 1)) (ok (cdr res) (+ i 2)) "
          "#f)))",
          "(define w1 (fb))", "(garbage 2000)", "(define w2 (fb))", "(garbage 2000)"})
        CHECK(cs.eval(d).has_value(), "4492 AC2: setup form evaluates");
    auto r2 = cs.eval("(if (ok w1 0) (ok w2 0) #f)");
    CHECK(r2 && is_bool(*r2) && as_bool(*r2),
          "4492 AC2: filter over a temporary list with allocating pred stays fresh");

    const auto src = read_file("src/compiler/evaluator_primitives_list.cpp");
    CHECK(src.find("TwVecPin pin_kept") != std::string::npos,
          "4492 AC3: filter holds a TwVecPin root over its snapshot/accumulator");
}

// ── Issue #4525: float allocation pressure vs #4474 whole-heap reclaim ──
// Spike verdict (see close comment): boxed floats are bump-allocated in
// lib/runtime.c's float_pool — OUTSIDE the mark heap — so pure float loops
// fire zero whole-heap marks (M1/M4: 200k iters → marks=0) and a float
// nursery would need float-liveness roots that do not exist (the pool is
// process-global across evaluators). The real burst cost is let/cons
// garbage re-firing the mark at the 128 floor cadence (M5: 200k-iter
// let-cell loop → 1562 marks ≈ +15% wall). Landed shape: the issue's
// minimum — a burst-escalating #4474 window (double when <half the
// window's mints survived), the 128 floor, live/32 scaling, and a
// ceiling that is heap-proportional since the #4474 third reopen
// (max(32768, live/4)) — small-heap bursts still clamp at 32768.
static void ac4525_burst_rebalance() {
    std::println("\n--- #4525: burst-escalating reclaim window ---");
    using clock = std::chrono::steady_clock;
    const auto ms = [](clock::duration d) {
        return std::chrono::duration_cast<std::chrono::microseconds>(d).count() / 1000.0;
    };

    // AC1: pure float loop drives ZERO whole-heap marks — boxed-scalar
    // allocation is decoupled from the #4474 window (post-#4502 float
    // pool). Pins the decoupling: a future change that counts float
    // boxes against the window trips this AC.
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        (void)ev.reclaim_tw_unreachable({}, {});
        const auto m0 = ev.file_heap_reclaim_runs_for_test();
        const auto t0 = clock::now();
        const auto r =
            cs.eval("(begin (define (lp i acc) (if (< i 50000) (lp (+ i 1) (+ acc 1.0)) acc))"
                    " (lp 0 0.0))");
        const auto wall = ms(clock::now() - t0);
        const auto marks = ev.file_heap_reclaim_runs_for_test() - m0;
        CHECK(r.has_value() && is_float(*r) && as_float(*r) == 50000.0,
              "4525 AC1: pure float loop returns 50000.0");
        CHECK(marks == 0, "4525 AC1: float loop fires zero whole-heap marks (got " +
                              std::to_string(marks) + ")");
        CHECK(ev.file_heap_allocs_since_reclaim() <= 4,
              "4525 AC1: float boxes do not advance the reclaim window (got " +
                  std::to_string(ev.file_heap_allocs_since_reclaim()) + " mints for 50k boxes)");
        std::println("    4525 AC1: 50k-iter float loop wall={:.1f}ms marks={}", wall, marks);
    }

    // AC2: burst escalation — a young-garbage let-cell loop re-fires the
    // mark at the floor cadence today (2000 mints ≈ 15 marks); the
    // #4525 window doubling cuts the mark count to ~4 while garbage
    // between reclaims stays bounded by the (doubled) trigger window.
    // Issue #4599: an uncaptured let no longer mints a cell. The burst
    // oracle set!s the binding so the let still allocates. Bounds stay.
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        (void)ev.reclaim_tw_unreachable({}, {});
        const auto m0 = ev.file_heap_reclaim_runs_for_test();
        const auto c0 = ev.cells_size();
        const auto t0 = clock::now();
        const auto r = cs.eval("(begin (define (lp i acc) (if (< i 2000) (lp (+ i 1) (let ((x (+ "
                               "acc 1.0))) (begin (set! x x) x)))"
                               " acc)) (lp 0 0.0))");
        const auto wall = ms(clock::now() - t0);
        const auto marks = ev.file_heap_reclaim_runs_for_test() - m0;
        const auto grown = ev.cells_size() - c0;
        CHECK(r.has_value() && is_float(*r) && as_float(*r) == 2000.0,
              "4525 AC2: let-cell burst loop returns 2000.0");
        CHECK(marks <= 8, "4525 AC2: burst loop mark count drops (got " + std::to_string(marks) +
                              ", floor cadence would be ~15)");
        CHECK(ev.file_heap_allocs_trigger_for_test() >= 512,
              "4525 AC2: window escalated past the floor (got " +
                  std::to_string(ev.file_heap_allocs_trigger_for_test()) + ")");
        CHECK(grown <= 2560,
              "4525 AC2: garbage stays ~one trigger window (grew " + std::to_string(grown) + ")");
        std::println("    4525 AC2: 2k-iter let-cell loop wall={:.1f}ms marks={} grown={}", wall,
                     marks, grown);
    }

    // AC3: escalation clamps at the #4474 ceiling — a long burst loop's
    // window saturates at 32768 and garbage between reclaims stays
    // bounded (the #4474 cap still bounds the nursery-ward garbage).
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        (void)ev.reclaim_tw_unreachable({}, {});
        const auto m0 = ev.file_heap_reclaim_runs_for_test();
        const auto c0 = ev.cells_size();
        const auto r = cs.eval("(begin (define (lp i acc) (if (< i 100000) (lp (+ i 1) (let ((x (+ "
                               "acc 1.0))) (begin (set! x x) x)))"
                               " acc)) (lp 0 0.0))");
        const auto marks = ev.file_heap_reclaim_runs_for_test() - m0;
        const auto grown = ev.cells_size() - c0;
        CHECK(r.has_value() && is_float(*r) && as_float(*r) == 100000.0,
              "4525 AC3: 100k-iter burst loop returns 100000.0");
        CHECK(ev.file_heap_allocs_trigger_for_test() == 32768,
              "4525 AC3: escalation clamps at the 32768 ceiling");
        CHECK(marks <= 20, "4525 AC3: ceiling window amortizes the mark (got " +
                               std::to_string(marks) + ", floor cadence would be ~781)");
        CHECK(grown <= 34816, "4525 AC3: garbage bounded by the 32768 ceiling (grew " +
                                  std::to_string(grown) + ")");
    }

    // AC4: live-dominant loops keep the plain #4474 scaling — no burst
    // doubling when the mark finds most of the window's mints alive.
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto fill = cs.eval("(begin (define v (make-vector 48000 0))"
                            "(define (f i) (if (< i 48000)"
                            "(begin (vector-set! v i (cons i i)) (f (+ i 1))) #f)) (f 0))");
        CHECK(fill.has_value(), "4525 AC4: 48k-pair live fill evals");
        (void)ev.reclaim_tw_unreachable({}, {});
        const auto trig0 = ev.file_heap_allocs_trigger_for_test();
        CHECK(trig0 >= 1400,
              "4525 AC4: scaled window from the 48k live set (got " + std::to_string(trig0) + ")");
        const auto m0 = ev.file_heap_reclaim_runs_for_test();
        const auto r = cs.eval("(begin (define (lp i acc) (if (< i 3000) (lp (+ i 1) (let ((x (+ "
                               "acc 1.0))) (begin (set! x x) x)))"
                               " acc)) (lp 0 0.0))");
        const auto marks = ev.file_heap_reclaim_runs_for_test() - m0;
        CHECK(r.has_value() && is_float(*r) && as_float(*r) == 3000.0,
              "4525 AC4: live-dominant burst loop returns 3000.0");
        CHECK(marks == 1, "4525 AC4: live-dominant loop marks at the scaled cadence (got " +
                              std::to_string(marks) + ", want 1)");
        CHECK(ev.file_heap_allocs_trigger_for_test() < 2 * trig0,
              "4525 AC4: no burst doubling when the live set dominates");
    }
}

// ── Issue #4350 / #4474: top-level closures must not copy all of top_ ──
// Every TW closure capture copied the whole capturing env, so K top-level
// procedure defines held ~K^2/2 frame bindings: each primitive reference
// (an index miss) scanned the copy (#4350), and the file-mode reclaim mark
// walked every copy per window, fired by each `let` cell (#4474). A
// top-level closure now captures an empty frame and reads globals through
// the live top_ consult.
static std::string defs_program(int k, int iters, const char* body) {
    std::string src = "(begin ";
    for (int i = 0; i < k; ++i)
        src += "(define (filler" + std::to_string(i) + " x) (+ x " + std::to_string(i) + "))";
    src += "(define (g x) (let ((r 1)) r))";
    src += "(define (run) (let loop ((i 0)) (if (< i " + std::to_string(iters) + ") (begin ";
    src += body;
    src += " (loop (+ i 1))) i)))";
    src += "(run))";
    return src;
}

static void ac4350_4474_top_level_capture() {
    std::println("\n--- #4350/#4474: top-level closures capture live top_ ---");
    constexpr int kDefs = 2000;
    // AC1: frame bindings stay O(K), not O(K^2).
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto r = cs.eval(defs_program(kDefs, 10, "0"));
        CHECK(r && is_int(*r) && as_int(*r) == 10, "4350 AC1: 2000-define program runs");
        std::size_t total = 0;
        for (std::size_t id = 0; id < ev.env_frames_size(); ++id) {
            const auto& fr = ev.env_frame(static_cast<std::uint32_t>(id) /* EnvId */);
            total += fr.bindings_symid_.size() + fr.bindings_.size();
        }
        CHECK(total < 20u * kDefs, "4350 AC1: frame bindings O(K) for K top-level defines (got " +
                                       std::to_string(total) +
                                       ", K^2/2 = " + std::to_string(kDefs * kDefs / 2) + ")");
    }
    // AC2: semantics through the live consult — forward references,
    // redefinition, set! of a global from a top-level closure.
    {
        CompilerService cs;
        auto r = cs.eval("(begin (define (use) (later 5)) (define (later x) (* x 2))"
                         "(define gx 1) (define (read-gx) gx) (define gx 40)"
                         "(define n 0) (define (bump!) (set! n (+ n 1)) n) (bump!) (bump!)"
                         "(+ (use) (read-gx) (bump!) n))");
        CHECK(r && is_int(*r) && as_int(*r) == 10 + 40 + 3 + 3,
              "4350 AC2: forward ref / redefine / global set! through live top_");
    }
    // AC3: per-iteration cost of a primitive-referencing loop and of a
    // `let` no longer grows with the number of top-level defines. Generous
    // bound (3x, best of 3) — pre-fix ratios were ~3.4x (empty loop) and
    // ~150x (`let`) at K=4000.
    for (const char* body : {"0", "(g 1)"}) {
        auto timed = [&](int k) {
            CompilerService cs;
            auto r0 = cs.eval(defs_program(k, 20000, body));
            if (!r0 || !is_int(*r0))
                return -1.0;
            double best = 1e18;
            for (int rep = 0; rep < 3; ++rep) {
                const auto t0 = std::chrono::steady_clock::now();
                auto r = cs.eval("(run)");
                const auto t1 = std::chrono::steady_clock::now();
                if (!r)
                    return -1.0;
                best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            return best;
        };
        const double small = timed(0);
        const double big = timed(4000);
        CHECK(small > 0 && big > 0, std::string("4350 AC3: loop evals (body ") + body + ")");
        CHECK(big < 3.0 * small + 5.0,
              std::string("4350/4474 AC3: K=4000 within 3x of K=0 (body ") + body + ", " +
                  std::to_string(big) + " ms vs " + std::to_string(small) + " ms)");
    }
    // AC4 (#4474): procedure defines in a required module share one capture
    // frame (letrec cells are identical for every closure in the module body)
    // instead of K copies of the module env; private helpers and forward
    // references still resolve.
    auto module_program = [](int k) {
        const std::string path = "/tmp/aura_4474_mod_" + std::to_string(k) + ".aura";
        std::ofstream mod(path);
        mod << "(export modfwd dummy1)\n(define (modfwd x) (modpriv (* x 2)))\n"
               "(define (modpriv x) (+ x 1))\n";
        // dummy1 is exported, so even the K=0 baseline defines two procs.
        for (int i = 0; i < std::max(k, 2); ++i)
            mod << "(define (dummy" << i << " x) (+ x " << i << "))\n";
        mod.close();
        return "(begin (require \"" + path +
               "\" all:) (define (g x) (let ((r 1)) r))"
               "(define (run) (let loop ((i 0)) (if (< i 20000) (begin (g 1) (loop (+ i 1))) i)))"
               "(+ (modfwd 5) (dummy1 1)))";
    };
    {
        constexpr int kMod = 2000;
        CompilerService cs;
        auto& ev = cs.evaluator();
        const auto before = ev.env_frames_size();
        auto r = cs.eval(module_program(kMod));
        CHECK(r && is_int(*r) && as_int(*r) == 11 + 2,
              "4474 AC4: module forward ref + private helper resolve");
        std::size_t total = 0;
        for (std::size_t id = before; id < ev.env_frames_size(); ++id) {
            const auto& fr = ev.env_frame(static_cast<std::uint32_t>(id) /* EnvId */);
            total += fr.bindings_symid_.size() + fr.bindings_.size();
        }
        CHECK(total < 20u * kMod, "4474 AC4: module frame bindings O(K) for K module procs (got " +
                                      std::to_string(total) + ")");
        auto timed_mod = [&](int k) {
            CompilerService c2;
            auto r0 = c2.eval(module_program(k));
            if (!r0 || !is_int(*r0))
                return -1.0;
            double best = 1e18;
            for (int rep = 0; rep < 3; ++rep) {
                const auto t0 = std::chrono::steady_clock::now();
                auto rr = c2.eval("(run)");
                const auto t1 = std::chrono::steady_clock::now();
                if (!rr)
                    return -1.0;
                best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            return best;
        };
        const double small = timed_mod(0);
        const double big = timed_mod(kMod);
        CHECK(small > 0 && big > 0, "4474 AC4: module let loop evals");
        CHECK(big < 3.0 * small + 5.0, "4474 AC4: module K=2000 let within 3x of K=0 (" +
                                           std::to_string(big) + " ms vs " + std::to_string(small) +
                                           " ms)");
    }
}

// ── Issue #4599: call-path name-resolution cache (phase 2) ──
// Phase 1 cached prim calls + cells per node. This phase finishes payoff
// step 1 on the call path: a body-pool frame binding for the callee name
// skips the resolve + string probes, the "not a prim" decision is cached
// per call node (host, generation/snapshot guarded, prim-table names
// refused), and and/or clause lists are cached. The load-robust oracle
// is the StringPool shared-lock reader counter: a hot self-TCO loop must
// perform zero pool resolves, and a loop with one materialized closure
// call per iteration must show only the bind mirror's one resolve.
static void ac4599_call_resolve_cache() {
    std::println("\n--- #4599: call-path resolve cache (phase 2) ---");
    // AC1: semantics preserved through every fast path (later calls of
    // each shape ride the cache).
    {
        CompilerService cs;
        auto r1 = cs.eval("(begin (define (gs4599 x) (let ((+ (lambda (a b) (* a b)))) (+ 3 4)))"
                          " (gs4599 1) (gs4599 1))");
        CHECK(r1 && is_int(*r1) && as_int(*r1) == 12,
              "4599 AC1: prim-valued let shadow keeps calling the lambda (12)");
        auto r2 = cs.eval("(begin (define (lp4599) (let loop ((i 0) (acc 0))"
                          " (if (< i 5) (loop (+ i 1) (+ acc 1)) acc)))"
                          " (lp4599) (lp4599))");
        CHECK(r2 && is_int(*r2) && as_int(*r2) == 5,
              "4599 AC1: self-TCO loop with local callee still iterates (5)");
        auto r3 = cs.eval("(begin (define (a4599) (if (and #f (car ())) 1 0))"
                          " (a4599) (a4599) (a4599))");
        CHECK(r3 && is_int(*r3) && as_int(*r3) == 0,
              "4599 AC1: cached and short-circuits before the error (0)");
        auto r4 = cs.eval("(begin (define (o4599) (if (or 7 (car ())) 4 9))"
                          " (o4599) (o4599) (o4599))");
        CHECK(r4 && is_int(*r4) && as_int(*r4) == 4,
              "4599 AC1: cached or short-circuits before the error (4)");
        auto r5 = cs.eval("(begin (define (q4599) (w4599)) (define (w4599) 1)"
                          " (q4599) (q4599))");
        CHECK(r5 && is_int(*r5) && as_int(*r5) == 1,
              "4599 AC1: top closure call through the cached decision (1)");
        auto r6 = cs.eval("(begin (define (w4599) 99) (q4599))");
        CHECK(r6 && is_int(*r6) && as_int(*r6) == 99,
              "4599 AC1: re-define is visible through the cell (99)");
    }

    // AC2: a late define invalidates the stored not-prim decision (top
    // bind generation bump) — the cached shape must not pin unbound.
    {
        CompilerService cs;
        auto r1 = cs.eval("(define (u4599) zz4599late)");
        CHECK(r1.has_value(), "4599 AC2: callee-first define lands");
        auto r2 = cs.eval("(u4599)");
        CHECK(!r2.has_value() || is_error(*r2), "4599 AC2: call before the late define fails");
        auto r3 = cs.eval("(define zz4599late 42)");
        CHECK(r3.has_value(), "4599 AC2: late define lands");
        auto r4 = cs.eval("(u4599)");
        CHECK(r4 && is_int(*r4) && as_int(*r4) == 42,
              "4599 AC2: late define visible after invalidation (42)");
    }

    // AC3: the hot self-TCO loop's per-iteration pool-read budget. The
    // call-path caches (cached and, local callee, cached prim args, the
    // not-a-prim decision) remove the prelude resolves; the ceiling here
    // covers the remaining per-visit machinery and is the number the
    // next phases ratchet down. The reader counter is process-global;
    // this thread is the only active evaluator here.
    {
        CompilerService cs;
        (void)cs.eval("(begin (define (run4599z) (let loop ((i 0))"
                      "   (if (and (<= 0 i) (< i 2000)) (loop (+ i 1)) i))))");
        auto warm = cs.eval("(run4599z)");
        CHECK(warm && is_int(*warm) && as_int(*warm) == 2000, "4599 AC3: warm loop runs");
        const auto t0 = aura::ast::stringpool_intern_concurrent_readers_total();
        auto hot = cs.eval("(run4599z)");
        const auto t1 = aura::ast::stringpool_intern_concurrent_readers_total();
        CHECK(hot && is_int(*hot) && as_int(*hot) == 2000, "4599 AC3: hot loop runs");
        CHECK(t1 - t0 <= 8ULL * 2000,
              "4599 AC3: hot loop pool reads stay within the 8/iter budget (got " +
                  std::to_string(t1 - t0) + " for 2000 iterations)");
    }

    // AC4: a loop with one materialized closure call per iteration pays
    // the materialize/bind path per call (payoff step 3, not this
    // phase); the prelude resolve + probes are silent after the cache
    // forms. 16 reads/iter is the measured ceiling, pinned so a
    // reintroduced per-visit string prelude cannot pass silently.
    {
        CompilerService cs;
        constexpr int kIters = 2000;
        (void)cs.eval("(begin (define (f1_4599c x) x)"
                      " (define (run4599c) (let loop ((i 0))"
                      "   (if (< i 2000) (begin (f1_4599c 1) (loop (+ i 1))) i))))");
        auto warm = cs.eval("(run4599c)");
        CHECK(warm && is_int(*warm) && as_int(*warm) == kIters, "4599 AC4: warm call loop runs");
        const auto t0 = aura::ast::stringpool_intern_concurrent_readers_total();
        auto hot = cs.eval("(run4599c)");
        const auto t1 = aura::ast::stringpool_intern_concurrent_readers_total();
        CHECK(hot && is_int(*hot) && as_int(*hot) == kIters, "4599 AC4: hot call loop runs");
        CHECK(t1 - t0 <= 16ULL * kIters,
              "4599 AC4: call loop pool reads stay within the 16/iter budget (got " +
                  std::to_string(t1 - t0) + " for " + std::to_string(kIters) + " calls)");
    }

    // AC5: relative budget — a 4000-define (f1 1) loop stays within 1.2x
    // of the 0-define loop (min of 3 reps each; the per-call work is
    // define-count independent after the cache). #4343/#4350/#4474.
    {
        using clock = std::chrono::steady_clock;
        auto prog = [](int k) {
            std::string s = "(begin (define (f1_4599d x) x)";
            for (int i = 0; i < k; ++i)
                s += "(define (fill4599_" + std::to_string(i) + " x) x)";
            s += " (define (run4599d) (let loop ((i 0))"
                 "   (if (< i 20000) (begin (f1_4599d 1) (loop (+ i 1))) i))) (run4599d))";
            return s;
        };
        // Interleaved reps: concurrent CI load inflates both sides; the
        // per-side min tracks the true ratio.
        CompilerService c_small;
        CompilerService c_big;
        auto r0s = c_small.eval(prog(0));
        auto r0b = c_big.eval(prog(4000));
        CHECK(r0s && is_int(*r0s) && r0b && is_int(*r0b), "4599 AC5: define-count loops eval");
        double small = 1e18, big = 1e18;
        for (int rep = 0; rep < 5; ++rep) {
            auto t0 = clock::now();
            auto rs = c_small.eval("(run4599d)");
            auto t1 = clock::now();
            if (rs && is_int(*rs))
                small = std::min(small, std::chrono::duration<double, std::milli>(t1 - t0).count());
            t0 = clock::now();
            auto rb = c_big.eval("(run4599d)");
            t1 = clock::now();
            if (rb && is_int(*rb))
                big = std::min(big, std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        CHECK(small > 0 && big > 0, "4599 AC5: both define-count loops eval");
        CHECK(big < 1.2 * small + 2.0, "4599 AC5: 4000-define loop within 1.2x of 0-define (" +
                                           std::to_string(big) + " ms vs " + std::to_string(small) +
                                           " ms)");
    }

    // AC6: the same-pool SymId chain read (phase 3) must keep the string
    // walk's exact answers. Every shape here rides the walk on its hot
    // visits (host, same-pool frames, aligned mirrors) and must still
    // resolve through cells, re-defined globals, keyword literals, and
    // unbound-name errors exactly like Env::lookup's string walk.
    {
        CompilerService cs;
        // Free var through the parent frame — the iters shape the walk
        // serves (integer compares, no pool resolve).
        auto r1 = cs.eval("(begin (define (f4599w n) (let loop ((i 0))"
                          "   (if (< i n) (loop (+ i 1)) i)))"
                          " (f4599w 7) (f4599w 7))");
        CHECK(r1 && is_int(*r1) && as_int(*r1) == 7,
              "4599 AC6: free var through the parent frame via the chain walk (7, got " +
                  (r1 ? std::to_string(as_int(*r1))
                      : std::string("nullopt: ") + std::string(r1.error().message)) +
                  ")");
        // set!-through-cell: the walk reads the cell, the deref stays exact.
        auto r2 = cs.eval("(begin (define (gs4599b) (let ((r 1))"
                          "   (let ((bump (lambda () (set! r 2)))) (begin (bump) r))))"
                          " (gs4599b) (gs4599b))");
        CHECK(r2 && is_int(*r2) && as_int(*r2) == 2,
              "4599 AC6: set!-through-cell stays exact through the walk (2, got " +
                  (r2 ? std::to_string(as_int(*r2)) : std::string("nullopt")) + ")");
        // A re-defined global stays visible (walk miss → string walk face).
        auto r3 = cs.eval("(begin (define x4599s 5) (define (sh4599) x4599s))");
        CHECK(r3.has_value(), "4599 AC6: global read lands");
        auto r4 = cs.eval("(sh4599)");
        CHECK(r4 && is_int(*r4) && as_int(*r4) == 5, "4599 AC6: global read via walk/fallback (5)");
        auto r5 = cs.eval("(define x4599s 9)");
        CHECK(r5.has_value(), "4599 AC6: re-define lands");
        auto r6 = cs.eval("(sh4599)");
        CHECK(r6 && is_int(*r6) && as_int(*r6) == 9,
              "4599 AC6: re-defined global visible through the chain (9)");
        // Keyword-named variables still take the keyword intern face.
        auto r7 = cs.eval("(begin (define (kw4599) :foo4599)"
                          "   (if (eq? (kw4599) :foo4599) 1 0))");
        CHECK(r7.has_value() && is_int(*r7) && as_int(*r7) == 1,
              "4599 AC6: keyword variable keeps the keyword intern identity");
        // Unbound names still produce the unbound Diagnostic.
        auto r8 = cs.eval("(begin (define (u4599z) no-such-4599z) (u4599z))");
        CHECK(!r8.has_value() || is_error(*r8),
              "4599 AC6: unbound name still errors through the walk");
        // Deep chain + let shadows between frames: nearest binding wins.
        auto r9 = cs.eval("(begin (define y4599 1)"
                          "   (define (d4599) (let ((y4599 2)) (let loop ((i 0))"
                          "      (if (< i 3) (loop (+ i 1)) y4599))))"
                          " (d4599) (d4599))");
        CHECK(r9 && is_int(*r9) && as_int(*r9) == 2,
              "4599 AC6: let shadow between chain frames wins (2, got " +
                  (r9 ? std::to_string(as_int(*r9)) : std::string("nullopt")) + ")");
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
    ac4474r3_large_heap_window();
    ac4480_map_accumulator_pinned();
    ac4492_nested_map_filter_pinned();
    ac4525_burst_rebalance();
    ac4350_4474_top_level_capture();
    ac4599_call_resolve_cache();
    std::println("\n=== #2651/#2997: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_pmr_alloc_fiber_safe();
}
#endif
