// @category: unit
// @reason: Issue #2651 / #2649 H9 — concurrent string_heap_ / pairs_ /
//          ASTArena allocate must not race pmr under multi-thread stress.
//
//   AC1: N threads push_string_heap large strings → no crash
//   AC2: N threads push_pair → no crash
//   AC3: concurrent string-append via eval under eval_mutex still works
//   AC4: source-cite #2651 on string-append + ASTArena alloc lock
//   AC5: push_string_heap / cons take alloc_storage_lock_

#include "test_harness.hpp"

#include <atomic>
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
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

} // namespace

int run_test_pmr_alloc_fiber_safe() {
    std::println("=== Issue #2651: PMR / string_heap concurrent alloc safety ===");
    ac1_concurrent_push_string();
    ac2_concurrent_push_pair();
    ac3_string_append_correct();
    ac4_source_cite();
    ac5_concurrent_eval_append();
    std::println("\n=== #2651: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_pmr_alloc_fiber_safe();
}
#endif
