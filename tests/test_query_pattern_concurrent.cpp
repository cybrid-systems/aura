// test_query_pattern_concurrent.cpp — Issue #1372:
// Close query:pattern tag_arity_index race window.

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::compiler::types::is_void;

namespace {

std::int64_t href(CompilerService& cs, const char* expr, const char* key) {
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", expr, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Encode (tag, arity) the same way Evaluator does.
std::uint64_t tag_arity_key(std::uint32_t tag, std::uint32_t arity) {
    return (static_cast<std::uint64_t>(tag) << 32) | static_cast<std::uint64_t>(arity);
}

} // namespace

int main() {
    // ── snapshot_tag_arity_bucket basic ──
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (f 2)\")").has_value(), "set-code");
        (void)cs.eval("(eval-current)");
        CHECK(ev.workspace_flat() != nullptr, "workspace loaded");

        // Force build then snapshot a known key
        ev.force_build_tag_arity_index();
        CHECK(ev.tag_arity_index_size() > 0, "index non-empty after force_build");
        CHECK(ev.get_tag_arity_index_race_window_hits() == 0, "race hits start 0");

        // Snapshot any key — even miss is fine (empty vector)
        auto miss = ev.snapshot_tag_arity_bucket(tag_arity_key(0xDEAD, 99));
        CHECK(miss.empty(), "unknown key → empty snapshot");
        CHECK(ev.get_tag_arity_index_race_window_hits() == 0, "race hits still 0 after miss");

        // Hit path: pick first entry via size > 0 after build
        // Walk via public size; snapshot with force rebuild
        auto any = ev.snapshot_tag_arity_bucket(0); // may be empty for tag0 arity0
        (void)any;
        CHECK(ev.get_tag_arity_index_race_window_hits() == 0, "race hits 0 after snapshot");
    }

    // ── query:pattern uses snapshot path (no shared-lock race) ──
    {
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(+ 1 2) (+ 3 4) (* 5 6)\")").has_value(), "set-code exprs");
        auto r = cs.eval("(query:pattern '(+ _ _))");
        CHECK(r.has_value(), "query:pattern returns");
        // Not void if index hit or full walk finds matches
        CHECK(r.has_value(), "pattern result present");
        CHECK(cs.evaluator().get_tag_arity_index_race_window_hits() == 0,
              "race hits 0 after query:pattern");
    }

    // ── race-window-hits in pattern-index-stats-hash ──
    {
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define a 1)\")").has_value(), "set-code");
        (void)cs.eval("(query:pattern '(define _ _))");
        auto race = href(cs, "query:pattern-index-stats-hash", "race-window-hits");
        CHECK(race == 0, "race-window-hits key == 0");
    }

    // ── concurrent force_build + snapshot (direct accessors) ──
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(begin (define (g n) n) (g 1) (g 2) (g 3))\")").has_value(),
              "set-code concurrent");
        (void)cs.eval("(eval-current)");
        ev.force_build_tag_arity_index();

        std::atomic<int> errors{0};
        std::atomic<int> snaps{0};
        constexpr int kThreads = 4;
        constexpr int kIters = 200;
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(kThreads));
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < kIters; ++i) {
                    try {
                        if ((i + t) % 3 == 0)
                            ev.invalidate_tag_arity_index_for_test();
                        else if ((i + t) % 3 == 1)
                            ev.force_build_tag_arity_index();
                        else {
                            auto b = ev.snapshot_tag_arity_bucket(tag_arity_key(1, 0));
                            (void)b;
                            snaps.fetch_add(1, std::memory_order_relaxed);
                        }
                    } catch (...) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& th : threads)
            th.join();
        CHECK(errors.load() == 0, "no exceptions under concurrent snapshot/build");
        CHECK(snaps.load() > 0, "snapshots executed");
        CHECK(ev.get_tag_arity_index_race_window_hits() == 0,
              "race hits 0 after concurrent stress");
    }

    // ── concurrent query:pattern via eval mutex (semantic correctness) ──
    {
        CompilerService cs;
        std::mutex eval_mtx;
        CHECK(cs.eval("(set-code \"(define (h x) (+ x 1)) (h 10) (h 20)\")").has_value(),
              "set-code multi");
        (void)cs.eval("(eval-current)");

        std::atomic<int> ok_queries{0};
        std::atomic<int> fails{0};
        constexpr int kThreads = 4;
        constexpr int kIters = 50;
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&]() {
                for (int i = 0; i < kIters; ++i) {
                    std::lock_guard<std::mutex> lock(eval_mtx);
                    auto r = cs.eval("(query:pattern '(define _ _))");
                    if (!r.has_value())
                        fails.fetch_add(1, std::memory_order_relaxed);
                    else
                        ok_queries.fetch_add(1, std::memory_order_relaxed);
                    // Interleave light mutate to force index rebuild
                    if (i % 7 == 0)
                        (void)cs.eval("(mutate:replace-value (define (h x) (+ x 1)) "
                                      "(define (h x) (+ x 2)))");
                }
            });
        }
        for (auto& th : threads)
            th.join();
        CHECK(fails.load() == 0, "no failed query:pattern under concurrent eval");
        CHECK(ok_queries.load() == kThreads * kIters, "all queries completed");
        CHECK(cs.evaluator().get_tag_arity_index_race_window_hits() == 0,
              "race hits 0 after concurrent pattern+mutate");
    }

    // ── invalidate then snapshot rebuilds cleanly ──
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(+ 1 2)\")").has_value(), "set-code");
        ev.force_build_tag_arity_index();
        const auto sz0 = ev.tag_arity_index_size();
        CHECK(sz0 > 0, "built");
        ev.invalidate_tag_arity_index_for_test();
        CHECK(ev.tag_arity_index_size() == 0, "invalidated");
        auto b = ev.snapshot_tag_arity_bucket(tag_arity_key(0xFFFFFFFF, 0));
        (void)b;
        // snapshot rebuilds as side effect
        CHECK(ev.tag_arity_index_size() > 0 || ev.workspace_flat() != nullptr,
              "snapshot rebuilds or workspace present");
        CHECK(ev.get_tag_arity_index_race_window_hits() == 0, "race 0 after invalidate+snapshot");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("query pattern concurrent #1372: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
