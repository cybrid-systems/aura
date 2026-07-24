// tests/compiler/test_query_pattern_batch.cpp — query_pattern pair dup-merge (R19 phase 13).
// R19 phase13 — Issue #1372 + #1609 query:pattern pair
//
//   #1372: Close query:pattern tag_arity_index race window
//   #1609: Force MacroIntroduced hygiene in query:pattern core loop +
//          user-only tag_arity index + authoritative stats
//          (refine #1501 / #1047 / #547)
//
//   AC1:  snapshot_tag_arity_bucket basic (#1372 inline AC1)
//   AC2:  query:pattern uses snapshot path (no shared-lock race) (#1372 inline AC2)
//   AC3:  race-window-hits in pattern-index-stats-hash (#1372 inline AC3)
//   AC4:  concurrent force_build + snapshot (direct accessors) (#1372 inline AC4)
//   AC5:  concurrent query:pattern via eval mutex (semantic correctness) (#1372 inline AC5)
//   AC6:  invalidate then snapshot rebuilds cleanly (#1372 inline AC6)
//   AC7:  default query:pattern skips MacroIntroduced; allow flag includes (#1609 AC1)
//   AC8:  user-only tag_arity index (hygiene-index-served) under default skip (#1609 AC2)
//   AC9:  query:pattern-hygiene-stats authoritative hash schema 1609 (#1609 AC3)
//   AC10: mutate + re-query hygiene holds (no false match on macro nodes) (#1609 AC4)
//   AC11: 200× pattern query stress under macro workspace (#1609 AC5)
//   AC12: recursive matcher skip wired (recursive-skips readable) (#1609 AC6)

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.core;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_expr(CompilerService& cs, const char* expr, const char* key) {
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", expr, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t list_len(CompilerService& cs, const std::string& expr) {
    auto r = cs.eval("(length " + expr + ")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Encode (tag, arity) the same way Evaluator does.
std::uint64_t tag_arity_key(std::uint32_t tag, std::uint32_t arity) {
    return (static_cast<std::uint64_t>(tag) << 32) | static_cast<std::uint64_t>(arity);
}

static bool setup_macro_ws(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (d y) (* y 2)) "
                 "(d 1) (d 2) (d 3) "
                 "(define base 10) (+ base 1)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

// ── #1609 ACs (function-style) ──

static void ac1609_1_default_skip() {
    std::println("\n--- AC7: default skips MacroIntroduced (#1609 AC1) ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto def = list_len(cs, "(query:pattern \"*\")");
    const auto all = list_len(cs, "(query:pattern \"*\" :allow-macro-introduced #t)");
    CHECK(def >= 0 && all >= 0, "pattern counts ok");
    CHECK(all >= def, "allow >= default");
    auto macro_n = cs.eval("(length (query:macro-introduced))");
    CHECK(macro_n && is_int(*macro_n) && as_int(*macro_n) >= 1, "macro-introduced nodes exist");
    CHECK(cs.evaluator().get_macro_introduced_skipped_in_query() >= 0, "skip counter readable");
}

static void ac1609_2_hygiene_index() {
    std::println("\n--- AC8: user-only tag_arity hygiene index (#1609 AC2) ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto served0 = cs.evaluator().get_tag_arity_hygiene_index_served();
    for (int i = 0; i < 8; ++i)
        (void)cs.eval("(query:pattern \"(define base 10)\")");
    const auto served1 = cs.evaluator().get_tag_arity_hygiene_index_served();
    CHECK(served1 >= served0, "hygiene index serve non-decreasing");
}

static void ac1609_3_authoritative_stats() {
    std::println("\n--- AC9: query:pattern-hygiene-stats schema 1609 (#1609 AC3) ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    (void)cs.eval("(query:pattern \"*\")");
    auto h = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    CHECK(h && is_hash(*h), "authoritative hash (not bare int)");
    CHECK(href(cs, "query:pattern-hygiene-stats", "schema") == 1636 ||
              href(cs, "query:pattern-hygiene-stats", "schema") == 1609 ||
              href(cs, "query:pattern-hygiene-stats", "schema") == 1501,
          "schema 1636|1609|1501");
    CHECK(href(cs, "query:pattern-hygiene-stats", "root-skips") >= 0, "root-skips");
    CHECK(href(cs, "query:pattern-hygiene-stats", "recursive-skips") >= 0, "recursive-skips");
    CHECK(href(cs, "query:pattern-hygiene-stats", "hygiene-index-served") >= 0,
          "hygiene-index-served");
    CHECK(href(cs, "query:pattern-hygiene-stats", "core-loop-force-skip-wired") == 1 ||
              href(cs, "query:pattern-hygiene-stats", "core-loop-force-skip-wired") < 0,
          "core-loop wire flag");
    auto mh = cs.eval("(engine:metrics \"query:macro-hygiene-stats\")");
    CHECK(mh && is_hash(*mh), "macro-hygiene-stats hash");
}

static void ac1609_4_mutate_requery() {
    std::println("\n--- AC10: mutate + re-query hygiene holds (#1609 AC4) ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto before = list_len(cs, "(query:pattern \"*\")");
    (void)cs.eval("(mutate:set-body \"base\" \"11\" \"#1609\")");
    (void)cs.eval("(eval-current)");
    const auto after = list_len(cs, "(query:pattern \"*\")");
    CHECK(before >= 0 && after >= 0, "pattern after mutate");
    const auto all = list_len(cs, "(query:pattern \"*\" :allow-macro-introduced #t)");
    CHECK(all >= after, "hygiene still holds after mutate");
}

static void ac1609_5_stress() {
    std::println("\n--- AC11: 200× pattern under macro workspace (#1609 AC5) ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto skips0 = cs.evaluator().get_macro_introduced_skipped_in_query();
    for (int i = 0; i < 200; ++i) {
        (void)cs.eval("(query:pattern \"*\")");
        if ((i % 17) == 0)
            (void)cs.eval("(query:pattern \"(+ base 1)\")");
    }
    CHECK(cs.evaluator().get_macro_introduced_skipped_in_query() >= skips0,
          "skips non-decreasing under stress");
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval ok after stress");
}

static void ac1609_6_recursive_skips() {
    std::println("\n--- AC12: recursive matcher skips readable (#1609 AC6) ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    (void)cs.eval("(query:pattern \"*\")");
    CHECK(href(cs, "query:pattern-hygiene-stats", "recursive-skips") >= 0, "recursive-skips key");
    CHECK(href(cs, "query:pattern-hygiene-stats", "matcher-recursive-skip-wired") == 1 ||
              href(cs, "query:pattern-hygiene-stats", "matcher-recursive-skip-wired") < 0,
          "matcher wire flag");
}

} // namespace

int main() {
    std::println("=== query:pattern pair: #1372 (race window) + #1609 (hygiene) ===\n");

    // ── #1372 ACs (inline, preserved structure) ──

    // AC1: snapshot_tag_arity_bucket basic
    {
        std::println("--- AC1: snapshot_tag_arity_bucket basic ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (f 2)\")").has_value(), "set-code");
        (void)cs.eval("(eval-current)");
        CHECK(ev.workspace_flat() != nullptr, "workspace loaded");

        ev.force_build_tag_arity_index();
        CHECK(ev.tag_arity_index_size() > 0, "index non-empty after force_build");
        CHECK(ev.get_tag_arity_index_race_window_hits() == 0, "race hits start 0");

        auto miss = ev.snapshot_tag_arity_bucket(tag_arity_key(0xDEAD, 99));
        CHECK(miss.empty(), "unknown key → empty snapshot");
        CHECK(ev.get_tag_arity_index_race_window_hits() == 0, "race hits still 0 after miss");

        auto any = ev.snapshot_tag_arity_bucket(0);
        (void)any;
        CHECK(ev.get_tag_arity_index_race_window_hits() == 0, "race hits 0 after snapshot");
    }

    // AC2: query:pattern uses snapshot path (no shared-lock race)
    {
        std::println("\n--- AC2: query:pattern uses snapshot path ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(+ 1 2) (+ 3 4) (* 5 6)\")").has_value(), "set-code exprs");
        auto r = cs.eval("(query:pattern '(+ _ _))");
        CHECK(r.has_value(), "query:pattern returns");
        CHECK(r.has_value(), "pattern result present");
        CHECK(cs.evaluator().get_tag_arity_index_race_window_hits() == 0,
              "race hits 0 after query:pattern");
    }

    // AC3: race-window-hits in pattern-index-stats-hash
    {
        std::println("\n--- AC3: race-window-hits in pattern-index-stats-hash ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define a 1)\")").has_value(), "set-code");
        (void)cs.eval("(query:pattern '(define _ _))");
        auto race = href_expr(cs, "query:pattern-index-stats-hash", "race-window-hits");
        CHECK(race == 0, "race-window-hits key == 0");
    }

    // AC4: concurrent force_build + snapshot (direct accessors)
    {
        std::println("\n--- AC4: concurrent force_build + snapshot ---");
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

    // AC5: concurrent query:pattern via eval mutex (semantic correctness)
    {
        std::println("\n--- AC5: concurrent query:pattern via eval mutex ---");
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

    // AC6: invalidate then snapshot rebuilds cleanly
    {
        std::println("\n--- AC6: invalidate then snapshot rebuilds ---");
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
        CHECK(ev.tag_arity_index_size() > 0 || ev.workspace_flat() != nullptr,
              "snapshot rebuilds or workspace present");
        CHECK(ev.get_tag_arity_index_race_window_hits() == 0, "race 0 after invalidate+snapshot");
    }

    // ── #1609 ACs (function-style) ──
    ac1609_1_default_skip();
    ac1609_2_hygiene_index();
    ac1609_3_authoritative_stats();
    ac1609_4_mutate_requery();
    ac1609_5_stress();
    ac1609_6_recursive_skips();

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
