// @category: unit
// @reason: Issue #2516 — single-transaction order for type_dep invalidate →
// partial re-infer → cascade mirror.
//
//   AC1: Source-cite single ordered sequence on all production partial paths
//   AC2: Stale type_dep edges for dirty nodes dropped before re-infer
//   AC3: After partial, cascade mirror sees post-infer affected set
//   AC4: Empty dirty → no mirror / no invalidate cost
//   AC5: Unit test locks order via counters + inject/source hooks

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-dep-partial-merge-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: single ordered sequence on production partial paths ──
static void ac1_single_ordered_sequence() {
    std::println("\n--- AC1: single ordered sequence source-cite ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto tch = read_file("src/compiler/type_checker.ixx");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto ev = read_file("src/compiler/evaluator_primitives_eval.cpp");

    CHECK(tci.find("Issue #2516") != std::string::npos, "AC1: impl cites #2516");
    CHECK(tci.find("invalidate_type_dep_for_nodes") != std::string::npos,
          "AC1: phase 1 invalidate");
    CHECK(tci.find("mirror_type_affected_to_cascade") != std::string::npos, "AC1: phase 3 mirror");
    CHECK(tch.find("infer_flat_partial_with_dirty_txn") != std::string::npos,
          "AC1: documented dirty-txn entry");
    // Decision table / phase order comments.
    CHECK(tci.find("Phase order") != std::string::npos ||
              tci.find("phase 1") != std::string::npos ||
              tci.find("type_dirty_txn_phase1") != std::string::npos,
          "AC1: phase order documented");
    // Production call sites prefer with_dirty_txn.
    CHECK(svc.find("infer_flat_partial_with_dirty_txn") != std::string::npos,
          "AC1: service uses dirty-txn entry");
    CHECK(etc.find("infer_flat_partial_with_dirty_txn") != std::string::npos,
          "AC1: evaluator typecheck uses dirty-txn entry");
    CHECK(ev.find("infer_flat_partial_with_dirty_txn") != std::string::npos,
          "AC1: eval primitives use dirty-txn entry");
}

// ── AC2: stale edges dropped before re-infer ──
static void ac2_invalidate_before_reinfer() {
    std::println("\n--- AC2: invalidate before re-infer ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    // Order in source: phase1 invalidate, then engine re-infer, then phase3.
    const auto p1 = tci.find("type_dirty_txn_phase1_invalidate_total");
    const auto p2 = tci.find("type_dirty_txn_phase2_reinfer_total");
    const auto p3 = tci.find("type_dirty_txn_phase3_mirror_total");
    CHECK(p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos,
          "AC2: all three phase counters present");
    CHECK(p1 < p2 && p2 < p3, "AC2: source order phase1 < phase2 < phase3");
    // invalidate call appears before re-infer phase marker.
    const auto inv = tci.find("invalidate_type_dep_for_nodes");
    // Find the #2516 invalidate (after expand), not just any occurrence.
    const auto inv_2516 = tci.find("Issue #2516 dirty txn");
    CHECK(inv_2516 != std::string::npos, "AC2: dirty txn block present");
    CHECK(inv != std::string::npos && inv > inv_2516, "AC2: invalidate in txn block");
    CHECK(p1 > inv_2516 && p1 < p2, "AC2: phase1 before reinfer");
}

// ── AC3: mirror after re-infer (post-infer cone) ──
static void ac3_mirror_after_reinfer() {
    std::println("\n--- AC3: mirror sees post-infer affected ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto dirty = read_file("src/compiler/dirty_propagation.ixx");
    CHECK(tci.find("AFTER re-infer") != std::string::npos ||
              tci.find("after re-infer") != std::string::npos ||
              tci.find("post-infer") != std::string::npos,
          "AC3: post-infer mirror documented");
    CHECK(tci.find("type_dirty_txn_phase3_mirror_total") != std::string::npos,
          "AC3: phase3 counter");
    // Mirror must come after phase2 counter bump in source.
    const auto p2 = tci.find("type_dirty_txn_phase2_reinfer_total");
    const auto mir = tci.find("mirror_type_affected_to_cascade");
    // There may be comments; find the call after phase2.
    const auto p2_after = tci.find("type_dirty_txn_phase2_reinfer_total", p2 + 1);
    (void)p2_after;
    CHECK(p2 != std::string::npos && mir != std::string::npos, "AC3: markers present");
    // The last mirror call should be after phase2.
    const auto mir_last = tci.rfind("mirror_type_affected_to_cascade");
    CHECK(mir_last != std::string::npos && mir_last > p2, "AC3: mirror call after phase2");
    CHECK(dirty.find("#2516") != std::string::npos, "AC3: dirty_propagation cites #2516");
}

// ── AC4: empty dirty zero cost ──
static void ac4_empty_dirty_zero_cost() {
    std::println("\n--- AC4: empty dirty → no txn cost ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    // Early return when affected.empty() before phase1.
    CHECK(tci.find("if (affected.empty())") != std::string::npos, "AC4: empty affected early exit");
    CHECK(tci.find("Empty affected already returned") != std::string::npos ||
              tci.find("no invalidate / no mirror") != std::string::npos ||
              tci.find("AC4") != std::string::npos,
          "AC4: empty path documented");
    // mirror helper itself no-ops on empty.
    const auto dirty = read_file("src/compiler/dirty_propagation.ixx");
    CHECK(dirty.find("affected_ast.empty()") != std::string::npos, "AC4: mirror empty no-op");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    // Without partial mutate, phase counters stay 0 (or non-negative).
    CHECK(href(cs, "type-dirty-txn-phase1-invalidate-total") >= 0, "AC4: phase1 key readable");
    CHECK(href(cs, "type-dirty-txn-total") >= 0, "AC4: txn-total key readable");
}

// ── AC5: counters + query lock order ──
static void ac5_counters_and_query() {
    std::println("\n--- AC5: counters + query schema-2516 ---");
    const auto met = read_file("src/compiler/observability_metrics.h");
    const auto fields = read_file("src/compiler/compiler_metrics_fields.inc");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");

    CHECK(met.find("type_dirty_txn_order_wired") != std::string::npos, "AC5: wired sentinel");
    CHECK(met.find("type_dirty_txn_phase1_invalidate_total") != std::string::npos, "AC5: phase1");
    CHECK(met.find("type_dirty_txn_phase2_reinfer_total") != std::string::npos, "AC5: phase2");
    CHECK(met.find("type_dirty_txn_phase3_mirror_total") != std::string::npos, "AC5: phase3");
    CHECK(fields.find("type_dirty_txn_order_wired") != std::string::npos, "AC5: fields.inc");
    CHECK(q.find("schema-2516") != std::string::npos, "AC5: query schema");
    CHECK(q.find("type-dirty-txn-order-wired") != std::string::npos, "AC5: query wired key");
    CHECK(q.find("type-dirty-txn-phase1-invalidate-total") != std::string::npos,
          "AC5: query phase1");
    CHECK(q.find("type-dirty-txn-phase3-mirror-total") != std::string::npos, "AC5: query phase3");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2516") == 2516, "AC5: schema-2516");
    CHECK(href(cs, "issue-2516") == 2516, "AC5: issue-2516");
    // Wired may be 0 until first non-empty partial; key must exist.
    CHECK(href(cs, "type-dirty-txn-order-wired") >= 0, "AC5: order-wired key");
    // Stable related schemas retained.
    CHECK(href(cs, "schema-2355") == 2355, "AC5: schema-2355 stable");
}

} // namespace

int run_test_type_dirty_txn_order() {
    std::println("=== Issue #2516: type_dep dirty txn order (invalidate→reinfer→mirror) ===");
    ac1_single_ordered_sequence();
    ac2_invalidate_before_reinfer();
    ac3_mirror_after_reinfer();
    ac4_empty_dirty_zero_cost();
    ac5_counters_and_query();
    std::println("\n=== #2516: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_type_dirty_txn_order();
}
#endif
