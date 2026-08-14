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
    CHECK(samples >= static_cast<std::uint64_t>(kThreads), "AC4: lock-hold samples recorded");

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
    CHECK(listp.find("kPrimHeapUnlimitedSmall") != std::string::npos,
          "AC3: small unlimited bypass");
    const auto hh = read_file("src/compiler/prim_heap_quota.hh");
    CHECK(hh.find("2997") != std::string::npos, "AC1: quota header notes #2997");
}

} // namespace

int run_test_pmr_alloc_fiber_safe() {
    std::println("=== Issue #2651: PMR / string_heap concurrent alloc safety ===");
    ac1_concurrent_push_string();
    ac2_concurrent_push_pair();
    ac3_string_append_correct();
    ac4_source_cite();
    ac5_concurrent_eval_append();
    ac6_concurrent_list_ctor();
    ac7_single_fiber_hotpath();
    std::println("\n=== #2651/#2997: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_pmr_alloc_fiber_safe();
}
#endif
