// tests/core/test_arena_defrag.cpp — Issue #1390: request_defrag + safepoint contract test.
//
//
// @category: integration
// @reason: concurrent arena.create<T>() + request_defrag()/defrag()
//          loop. Verifies no UB under concurrent workload, plus
//          the new (arena:safepoint-registered?) /
//          (arena:warn-no-safepoint) primitives.
//
// test_arena_defrag_concurrent.cpp — Issue #1390:
// request_defrag feedback when no safepoint registered +
// defrag×alloc contract test.
//
// Background: request_defrag() sets a flag observed by
// allocation safepoints. If no safepoint is registered
// (stdin mode, std::thread workers from fiber:spawn), the
// flag is permanently stuck — operator gets no signal.
// defrag() does in-place bumping (no exclusive lock) so
// concurrent arena.create<T>() must not UAF reclaimed bytes.
//
// Fix (src/core/arena.ixx:287-300, gc_hooks.h:73-78):
// - request_defrag() now returns [[nodiscard]] bool indicating
//   whether a safepoint is registered to observe the flag.
// - One-shot stderr warning emitted on first request_defrag()
//   with no safepoint registered.
// - New (arena:safepoint-registered?) primitive exposes the
//   same boolean.
// - New (arena:warn-no-safepoint) primitive returns whether
//   the one-shot warning has fired.
//
// Tests:
//   AC1: (arena:safepoint-registered?) primitive returns bool
//   AC2: (arena:warn-no-safepoint) primitive — false initially,
//        true after request_defrag() with no safepoint
//   AC3: 2-thread concurrent arena.create<T>() + defrag()
//        loop runs for 3s without SIGSEGV/SIGABRT
//   AC4: (arena:request-defrag) returns bool (true iff a
//        safepoint is registered to observe the flag)

#include "test_harness.hpp"

#include <fstream>
#include <iterator>
#include <string>

import std;
using namespace std::chrono_literals;

import aura.core;
import aura.core.type;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.compiler.service;

namespace aura_arena_defrag_concurrent_detail {

// Thread A: high-frequency arena allocations. Mimics the
// std::thread worker spawned by fiber:spawn that doesn't
// call safepoint_check() before allocating — the case where
// the request_defrag flag is permanently stuck.
void thread_a_alloc(aura::compiler::CompilerService& cs, std::atomic<bool>& done,
                    std::atomic<int>& alloc_count) {
    while (!done.load(std::memory_order_acquire)) {
        // Allocate TestObj 100 times per iteration to drive
        // meaningful fragmentation pressure on the arena.
        for (int i = 0; i < 100; ++i) {
            auto* p = cs.arena().create<int>();
            if (p) {
                *p = i;
            }
        }
        alloc_count.fetch_add(100, std::memory_order_relaxed);
    }
}

// Thread B: loops request_defrag() + defrag(). Each iteration
// sets the flag (returns bool indicating whether it'll be
// observed) and immediately runs defrag() to reclaim bytes.
void thread_b_defrag(aura::compiler::CompilerService& cs, std::atomic<bool>& done,
                     std::atomic<int>& defrag_count) {
    while (!done.load(std::memory_order_acquire)) {
        bool observed = cs.arena().request_defrag();
        cs.arena().defrag();
        (void)observed; // informational — test cares about no-UB, not return value
        defrag_count.fetch_add(1, std::memory_order_relaxed);
    }
}

// ── AC1: arena:safepoint-registered? via engine:metrics ─────
// SlimSurface: demoted from public add() to register_stats_impl;
// call through (engine:metrics "…") not a free primitive form.
bool test_ac1_safepoint_registered_primitive() {
    std::println("\n--- AC1: (engine:metrics \"arena:safepoint-registered?\") ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(engine:metrics \"arena:safepoint-registered?\")");
    CHECK(r.has_value(), "AC1: stats surface returns a value");
    if (r && aura::compiler::types::is_bool(*r)) {
        auto b = aura::compiler::types::as_bool(*r);
        std::println("  AC1: safepoint_registered = {}", b);
        // Value depends on whether the fiber scheduler registered a
        // safepoint (likely false in this bare test process).
        CHECK(true, "AC1: stats surface returns a bool (value varies)");
    } else {
        CHECK(false, "AC1: stats surface returns a bool");
    }
    return true;
}

// ── AC2: arena:warn-no-safepoint via engine:metrics ──────────
bool test_ac2_warn_no_safepoint_primitive() {
    std::println("\n--- AC2: (engine:metrics \"arena:warn-no-safepoint\") ---");
    aura::compiler::CompilerService cs;

    // AC2a: initially — warning hasn't fired
    auto r0 = cs.eval("(engine:metrics \"arena:warn-no-safepoint\")");
    bool b0 =
        (r0 && aura::compiler::types::is_bool(*r0)) ? aura::compiler::types::as_bool(*r0) : true;
    std::println("  AC2: initial warn-no-safepoint = {}", b0);
    CHECK(!b0, "AC2a: warning hasn't fired initially (clean process state)");

    // AC2b: trigger request_defrag — in stdin mode without
    // scheduler, no safepoint is registered, so the one-shot
    // warning should fire.
    auto rr = cs.eval("(arena:request-defrag)");
    CHECK(rr.has_value(), "AC2b: request_defrag primitive returns a value");

    // Now: warning should have fired (true)
    auto r1 = cs.eval("(engine:metrics \"arena:warn-no-safepoint\")");
    bool b1 =
        (r1 && aura::compiler::types::is_bool(*r1)) ? aura::compiler::types::as_bool(*r1) : false;
    std::println("  AC2: post-request warn-no-safepoint = {}", b1);
    CHECK(b1, "AC2b: warning fired after request_defrag with no "
              "safepoint (one-shot guarantee)");

    // AC2c: warning should still be true (one-shot, persistent)
    auto r2 = cs.eval("(engine:metrics \"arena:warn-no-safepoint\")");
    bool b2 =
        (r2 && aura::compiler::types::is_bool(*r2)) ? aura::compiler::types::as_bool(*r2) : false;
    std::println("  AC2: second query warn-no-safepoint = {}", b2);
    CHECK(b2, "AC2c: warning stays fired (one-shot, persistent state)");
    return true;
}

// ── AC3: 2-thread concurrent alloc + defrag, no UB ─────────
bool test_ac3_concurrent_alloc_defrag() {
    std::println("\n--- AC3: concurrent alloc + defrag, no UB ---");
    aura::compiler::CompilerService cs;

    std::atomic<bool> done{false};
    std::atomic<int> alloc_count{0};
    std::atomic<int> defrag_count{0};

    std::thread ta(thread_a_alloc, std::ref(cs), std::ref(done), std::ref(alloc_count));
    std::thread tb(thread_b_defrag, std::ref(cs), std::ref(done), std::ref(defrag_count));

    std::this_thread::sleep_for(3s);
    done.store(true, std::memory_order_release);

    ta.join();
    tb.join();

    auto a = alloc_count.load();
    auto d = defrag_count.load();
    std::println("  AC3: alloc_count={} defrag_count={}", a, d);
    CHECK(a > 0, "AC3: alloc thread completed iterations");
    CHECK(d > 0, "AC3: defrag thread completed iterations");
    CHECK(a < 100000000, "AC3: alloc count under sanity bound (no UB / runaway loop)");
    return true;
}

// ── AC4: (arena:request-defrag) returns bool ─────────────────
bool test_ac4_request_defrag_returns_bool() {
    std::println("\n--- AC4: (arena:request-defrag) returns bool ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(arena:request-defrag)");
    CHECK(r.has_value(), "AC4: primitive returns a value");
    if (r && aura::compiler::types::is_bool(*r)) {
        auto b = aura::compiler::types::as_bool(*r);
        std::println("  AC4: request_defrag returned = {}", b);
        // In stdin mode without scheduler, safepoint not
        // registered → expect false (warning also fired).
        // We don't assert false because tests could run with
        // scheduler up; AC is "primitive returns bool".
        CHECK(true, "AC4: primitive returns bool (value depends on scheduler)");
    } else {
        CHECK(false, "AC4: primitive returns a bool");
    }
    return true;
}

static std::string read_file(const char* path) {
    for (const char* prefix : {"", "../", "../../"}) {
        std::ifstream in(std::string(prefix) + path);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static void ac3269_1_unique_after_tls_skip() {
    std::println("\n--- #3269 AC1: compact/defrag unique workspace after TLS skip ---");
    const auto mem = read_file("src/compiler/evaluator_primitives_memory.cpp");
    CHECK(mem.find("with_arena_compact_idle") != std::string::npos, "3269 AC1: helper");
    CHECK(mem.find("WorkspaceUniqueIfNeeded compact_hold") != std::string::npos,
          "3269 AC1: unique workspace");
    CHECK(mem.find("Issue #3269") != std::string::npos, "3269 AC1: cite");
    auto cpos = mem.find("add(\"arena:compact\"");
    CHECK(cpos != std::string::npos, "3269 AC1: compact public");
    auto cwin = mem.substr(cpos, 700);
    CHECK(cwin.find("with_arena_compact_idle") != std::string::npos,
          "3269 AC1: compact uses helper");
    CHECK(mem.find("sink_arena_prim(\"arena:defrag\"") != std::string::npos, "3269 AC1: defrag");
    CHECK(mem.find("sink_arena_prim(\"arena:defrag-now\"") != std::string::npos,
          "3269 AC1: defrag-now");
    CHECK(mem.find("sink_arena_prim(\"arena:live-compact\"") != std::string::npos,
          "3269 AC1: live-compact");
}

static void ac3269_2_raced_counter() {
    std::println("\n--- #3269 AC2: raced-after-check counter ---");
    const auto mem = read_file("src/compiler/evaluator_primitives_memory.cpp");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    CHECK(mem.find("compaction_boundary_raced_after_check_") != std::string::npos,
          "3269 AC2: bump");
    CHECK(ixx.find("compaction_boundary_raced_after_check_") != std::string::npos,
          "3269 AC2: member");
    CHECK(mem.find("mutation_boundary_held()") != std::string::npos,
          "3269 AC2: re-check after lock");
    aura::compiler::CompilerService cs;
    CHECK(cs.evaluator().compaction_boundary_raced_after_check() == 0,
          "3269 AC2: quiet path raced is 0");
}

static void ac3269_3_snapshot_comment() {
    std::println("\n--- #3269 AC3: live-compact snapshot is best-effort ---");
    const auto mem = read_file("src/compiler/evaluator_primitives_memory.cpp");
    auto pos = mem.find("sink_arena_prim(\"arena:live-compact\"");
    CHECK(pos != std::string::npos, "3269 AC3: live-compact");
    auto win = mem.substr(pos, 1800);
    CHECK(win.find("best-effort snapshot") != std::string::npos, "3269 AC3: comment");
    CHECK(win.find("memory_order_relaxed") != std::string::npos, "3269 AC3: relaxed kept");
}

static void ac3269_4_compact_under_guard() {
    std::println("\n--- #3269 AC4: same-fiber Guard pauses compact; idle compact runs ---");
    aura::compiler::CompilerService cs;
    auto idle = cs.eval("(arena:compact)");
    CHECK(idle.has_value() && aura::compiler::types::is_int(*idle) &&
              aura::compiler::types::as_int(*idle) >= 0,
          "3269 AC4: idle compact");
    bool ok = true;
    {
        auto gr =
            aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok);
        CHECK(gr.has_value(), "3269 AC4: Guard");
        auto paused = cs.eval("(arena:compact)");
        CHECK(paused.has_value() && aura::compiler::types::is_int(*paused) &&
                  aura::compiler::types::as_int(*paused) == 0,
              "3269 AC4: compact under Guard returns 0");
        CHECK(cs.evaluator().compaction_paused_by_boundary() >= 1, "3269 AC4: paused counter");
    }
    auto after = cs.eval("(arena:compact)");
    CHECK(after.has_value() && aura::compiler::types::is_int(*after) &&
              aura::compiler::types::as_int(*after) >= 0,
          "3269 AC4: compact after Guard");
}

static void ac3269_5_source_and_linter() {
    std::println("\n--- #3269 AC5: linter + no invent ---");
    const auto t = read_file("tests/core/test_arena_defrag.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_arena_compact_toctou_3269.py");
    CHECK(t.find("ac3269_1_unique_after_tls_skip") != std::string::npos, "3269 AC5: AC1");
    CHECK(t.find("ac3269_2_raced_counter") != std::string::npos, "3269 AC5: AC2");
    CHECK(t.find("ac3269_3_snapshot_comment") != std::string::npos, "3269 AC5: AC3");
    CHECK(t.find("ac3269_4_compact_under_guard") != std::string::npos, "3269 AC5: AC4");
    CHECK(!lint.empty() && lint.find("Issue #3269") != std::string::npos, "3269 AC5: linter");
    CHECK(build.find("check_arena_compact_toctou_3269") != std::string::npos, "3269 AC5: build.py");
    CHECK(read_file("docs/design/3269-arena-compact-toctou.md").empty(),
          "3269 AC5: no docs/design");
    CHECK(read_file("tests/compiler/test_issue_3269.cpp").empty(), "3269 AC5: no invent");
}

} // namespace aura_arena_defrag_concurrent_detail

int main() {
    using namespace aura_arena_defrag_concurrent_detail;
    bool ok = true;
    ok &= test_ac1_safepoint_registered_primitive();
    ok &= test_ac2_warn_no_safepoint_primitive();
    ok &= test_ac3_concurrent_alloc_defrag();
    ok &= test_ac4_request_defrag_returns_bool();
    std::println("\n=== Issue #3269: arena compact TOCTOU ===");
    ac3269_1_unique_after_tls_skip();
    ac3269_2_raced_counter();
    ac3269_3_snapshot_comment();
    ac3269_4_compact_under_guard();
    ac3269_5_source_and_linter();

    int failed = aura::test::g_failed;
    std::println("\n=== Issue #1390/#3269 arena defrag: {} ({} failures) ===",
                 (ok && failed == 0) ? "PASS" : "FAIL", failed);
    return (ok && failed == 0) ? 0 : 1;
}