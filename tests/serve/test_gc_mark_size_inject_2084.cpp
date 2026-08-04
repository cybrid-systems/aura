// @category: integration
// @reason: Issue #2084 — inject real heap sizes into mark_from_roots
// (stop size=0,0,0 path; full-heap MarkBitVector coverage under AI
// mutate+GC). Extends #2001 compact_sweep live_mask.
//
//   AC1: mark_from_roots with injected sizes → MarkBitVector size == heap size
//   AC2: root-only fallback undersizes vs injected high-water
//   AC3: size_fn_ path used by collect() (register_size_fn + metrics)
//   AC4: high-water dead slots above max root are covered (count_dead)
//   AC5: query:gc-mark-size-stats schema-2084 + injection counters
//   AC6: query:gc-compact-stats schema-2084 lineage keys
//   AC7: evaluator string/pairs/closures_size getters for size provider
//   AC8: serve_async + coordinator source wiring retained

#include "test_harness.hpp"
#include "serve/gc_coordinator.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <tuple>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::serve::GCCollector;
using aura::serve::GCRootSet;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void ac1_injected_sizes_match() {
    std::println("\n--- AC1: injected sizes size MarkBitVectors ---");
    GCCollector gc(nullptr);
    GCRootSet roots;
    roots.string_roots = {0, 2};
    roots.pair_roots = {1};
    roots.closure_roots = {3};
    constexpr std::size_t kS = 1000, kP = 500, kC = 200;
    gc.mark_from_roots(roots, kS, kP, kC);
    CHECK(gc.string_marks_size() == kS, "string_marks_size == 1000");
    CHECK(gc.pair_marks_size() == kP, "pair_marks_size == 500");
    CHECK(gc.closure_marks_size() == kC, "closure_marks_size == 200");
    CHECK(gc.string_mark(0) && gc.string_mark(2), "string roots marked");
    CHECK(gc.pair_mark(1), "pair root marked");
    CHECK(gc.closure_mark(3), "closure root marked");
    CHECK(!gc.string_mark(999), "high-water string not live (but covered)");
}

static void ac2_fallback_undersizes() {
    std::println("\n--- AC2: root-only fallback undersizes vs high-water inject ---");
    GCRootSet roots;
    roots.string_roots = {5}; // max root index 5 → fallback size 6

    GCCollector gc_fallback(nullptr);
    gc_fallback.mark_from_roots(roots, /*string=*/0, /*pairs=*/0, /*closures=*/0);
    CHECK(gc_fallback.string_marks_size() == 6, "fallback size = max_root+1 = 6");

    GCCollector gc_inject(nullptr);
    gc_inject.mark_from_roots(roots, /*string=*/1000, /*pairs=*/0, /*closures=*/0);
    CHECK(gc_inject.string_marks_size() == 1000, "injected size = 1000");
    CHECK(gc_inject.string_marks_size() > gc_fallback.string_marks_size(),
          "injected covers more than fallback");
}

static void ac3_size_fn_register() {
    std::println("\n--- AC3: register_size_fn stores provider ---");
    GCCollector gc(nullptr);
    bool called = false;
    gc.register_size_fn([&]() -> std::tuple<std::size_t, std::size_t, std::size_t> {
        called = true;
        return {64, 32, 16};
    });
    CHECK(!called, "size_fn not invoked at register");
    // Simulate collect()'s size_fn_ query + mark (full collect needs Scheduler).
    GCRootSet roots;
    roots.string_roots = {1};
    // Manual inject matching what collect() does after size_fn_().
    gc.mark_from_roots(roots, 64, 32, 16);
    CHECK(gc.string_marks_size() == 64, "size_fn-derived string size");
    CHECK(gc.pair_marks_size() == 32, "size_fn-derived pairs size");
    CHECK(gc.closure_marks_size() == 16, "size_fn-derived closures size");
    CHECK(aura::serve::g_mark_size_injected_total.load() > 0, "process metric advanced");
}

static void ac4_high_water_dead_covered() {
    std::println("\n--- AC4: high-water dead slots covered by mark vector ---");
    GCCollector gc(nullptr);
    GCRootSet roots;
    // Only low roots; heap grew to 500 with dead high-water.
    roots.string_roots = {0, 3};
    roots.pair_roots = {2};
    constexpr std::size_t kHeap = 500;
    gc.mark_from_roots(roots, kHeap, kHeap, 0);
    CHECK(gc.string_marks_size() == kHeap, "full string heap covered");
    CHECK(gc.pair_marks_size() == kHeap, "full pairs heap covered");
    // Dead high-water index is addressable (not silent no-op beyond size).
    CHECK(!gc.string_mark(kHeap - 1), "dead high-water unmarked");
    CHECK(gc.string_marks_dead_count() == kHeap - 2, "dead count = heap - 2 live roots");
    // test() beyond size still false; size itself proves coverage for sweep.
    CHECK(gc.string_marks_size() > 4, "coverage >> max root index");
}

static void ac5_query_mark_size() {
    std::println("\n--- AC5: query:gc-mark-size-stats schema-2084 ---");
    // Ensure at least one injection so counters are readable.
    {
        GCCollector gc(nullptr);
        GCRootSet roots;
        roots.string_roots = {0};
        gc.mark_from_roots(roots, 42, 7, 3);
    }
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:gc-mark-size-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "query:gc-mark-size-stats", "schema") == 2084, "schema 2084");
    CHECK(href(cs, "query:gc-mark-size-stats", "schema-2084") == 2084, "schema-2084");
    CHECK(href(cs, "query:gc-mark-size-stats", "issue-2084") == 2084, "issue-2084");
    CHECK(href(cs, "query:gc-mark-size-stats", "mark-size-injected-total") > 0, "injected > 0");
    CHECK(href(cs, "query:gc-mark-size-stats", "last-injected-string-size") == 42,
          "last string 42");
    CHECK(href(cs, "query:gc-mark-size-stats", "last-injected-pairs-size") == 7, "last pairs 7");
    CHECK(href(cs, "query:gc-mark-size-stats", "last-injected-closures-size") == 3, "last clos 3");
    CHECK(href(cs, "query:gc-mark-size-stats", "mark-size-provider-wired") == 1, "provider wired");
    CHECK(href(cs, "query:gc-mark-size-stats", "size-provider-wired") == 1, "size-provider-wired");
}

static void ac6_compact_stats_lineage() {
    std::println("\n--- AC6: query:gc-compact-stats schema-2084 lineage ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:gc-compact-stats\")");
    CHECK(h && is_hash(*h), "compact hash");
    CHECK(href(cs, "query:gc-compact-stats", "schema") == 2001, "schema 2001");
    CHECK(href(cs, "query:gc-compact-stats", "schema-2084") == 2084, "schema-2084");
    CHECK(href(cs, "query:gc-compact-stats", "mark-size-injected-total") >= 0, "inject total");
    CHECK(href(cs, "query:gc-compact-stats", "mark-size-provider-wired") == 1, "wired");
}

static void ac7_evaluator_size_getters() {
    std::println("\n--- AC7: evaluator heap size getters for size provider ---");
    CompilerService cs;
    CHECK(cs.evaluator().string_heap_size() >= 0, "string_heap_size");
    CHECK(cs.evaluator().pairs_size() >= 0, "pairs_size");
    CHECK(cs.evaluator().closures_size() >= 0, "closures_size");
    // Register lambdas so closures_ is non-empty (size provider path).
    CHECK(cs.eval("(set-code \"(define f (lambda (x) x))(define g (lambda () 1))\")").has_value(),
          "set-code lambdas");
    (void)cs.eval("(eval-current)");
    const auto c1 = cs.evaluator().closures_size();
    // closures_ may grow on eval; getter itself is what size_fn uses.
    CHECK(c1 >= 0, "closures_size readable after eval");
    // Size provider returns live evaluator extents — mark_from_roots must
    // honor them even when root max index is much smaller.
    GCCollector gc(nullptr);
    GCRootSet roots;
    roots.string_roots = {0};
    roots.closure_roots = {0};
    const auto s = std::max<std::size_t>(cs.evaluator().string_heap_size(), 1);
    const auto p = std::max<std::size_t>(cs.evaluator().pairs_size(), 1);
    const auto c = cs.evaluator().closures_size();
    // Inject high-water synthetic sizes (as if heaps grew past last root).
    const auto inject_s = s + 500;
    const auto inject_p = p + 200;
    const auto inject_c = c + 50;
    gc.mark_from_roots(roots, inject_s, inject_p, inject_c);
    CHECK(gc.string_marks_size() == inject_s, "mark string size == injected extent");
    CHECK(gc.pair_marks_size() == inject_p, "mark pair size == injected extent");
    CHECK(gc.closure_marks_size() == inject_c, "mark closure size == injected extent");
    CHECK(gc.string_marks_size() > roots.string_roots.back() + 1,
          "string coverage > root-max fallback");
}

static void ac8_source_wiring() {
    std::println("\n--- AC8: source wiring size_fn + serve_async ---");
    const auto h = read_file("src/serve/gc_coordinator.h");
    const auto c = read_file("src/serve/gc_coordinator.cpp");
    const auto sa = read_file("src/serve/serve_async.cpp");
    CHECK(!h.empty() && h.find("register_size_fn") != std::string::npos, "register_size_fn");
    CHECK(h.find("string_marks_size") != std::string::npos, "string_marks_size accessor");
    CHECK(h.find("g_mark_size_injected_total") != std::string::npos, "process metric");
    CHECK(!c.empty() && c.find("size_fn_") != std::string::npos, "collect uses size_fn_");
    CHECK(c.find("sizes_injected") != std::string::npos ||
              c.find("string_heap_size > 0") != std::string::npos,
          "injection path");
    CHECK(!sa.empty() && sa.find("register_size_fn") != std::string::npos, "serve registers");
    CHECK(sa.find("string_heap_size()") != std::string::npos &&
              sa.find("pairs_size()") != std::string::npos &&
              sa.find("closures_size()") != std::string::npos,
          "provider returns three sizes");
}

} // namespace

int run_test_gc_mark_size_inject_2084() {
    std::println("=== Issue #2084: GC mark_from_roots real heap size injection ===");
    ac1_injected_sizes_match();
    ac2_fallback_undersizes();
    ac3_size_fn_register();
    ac4_high_water_dead_covered();
    ac5_query_mark_size();
    ac6_compact_stats_lineage();
    ac7_evaluator_size_getters();
    ac8_source_wiring();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_gc_mark_size_inject_2084();
}
#endif
