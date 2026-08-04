// @category: unit
// @reason: Issue #2355 — type_dep_graph_ epoch prune + NodeId invalidation
// under long AI sessions (complements #2320 live-AST prune / #2283 merge).
//
//   AC1: After set_cache_epoch(e+1), edges stamped at epoch e (e>0) drop;
//        epoch-0 sentinel edges survive.
//   AC2: N rounds of record under advancing epochs → edge count bounded
//        (not O(session length) unbounded).
//   AC3: Same epoch / empty invalidate → no extra stale-drop atomics.
//   AC4: Schema-2355 additive; #2320/#2283 keys preserved.
//   AC5: Source-cite prune/invalidate/record + unit matrix.

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.ast;
import aura.core.type;

namespace {

using aura::ast::NodeId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::g_type_dep_graph_stale_drop_total;
using aura::compiler::kTypeDepBucketCap;
using aura::compiler::TypeChecker;
using aura::compiler::TypeDepEdge;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;
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

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-dep-partial-merge-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: epoch prune drops old stamped edges; epoch-0 survives ──
static void ac1_epoch_prune() {
    std::println("\n--- AC1: set_cache_epoch drops older stamped edges ---");
    TypeRegistry reg;
    TypeChecker tc(reg);
    CompilerMetrics metrics;
    tc.set_metrics(&metrics);

    tc.set_cache_epoch(1);
    tc.record_type_dependency(/*tid=*/42, /*node=*/10);
    tc.record_type_dependency(/*tid=*/42, /*node=*/11);
    // Untagged-style: force epoch-0 by recording at epoch 0 first.
    tc.set_cache_epoch(0);
    tc.record_type_dependency(/*tid=*/42, /*node=*/99); // epoch-0 sentinel
    CHECK(tc.type_dep_graph_edge_count() == 3, "AC1: 3 edges before prune");

    const auto drop0 = g_type_dep_graph_stale_drop_total.load();
    // Advance to epoch 2 → drop edges with epoch in (0, 2) i.e. epoch==1.
    tc.set_cache_epoch(2);
    CHECK(tc.type_dep_graph_edge_count() == 1, "AC1: only epoch-0 edge survives");
    CHECK(g_type_dep_graph_stale_drop_total.load() >= drop0 + 2, "AC1: stale_drop bumped by ~2");
    auto live = tc.affected_nodes_for_type(42);
    CHECK(live.size() == 1 && live[0] == 99, "AC1: survivor nid == 99 (epoch-0)");
}

// ── AC2: multi-round rebind bound ──
static void ac2_session_bound() {
    std::println("\n--- AC2: multi-round record under advancing epochs is bounded ---");
    TypeRegistry reg;
    TypeChecker tc(reg);
    const std::uint32_t tid = 7;
    // Simulate N rounds: each round stamps a few nodes then advances epoch.
    for (std::uint64_t e = 1; e <= 50; ++e) {
        tc.set_cache_epoch(e);
        for (NodeId n = 0; n < 8; ++n)
            tc.record_type_dependency(tid, static_cast<NodeId>(n + e * 100));
    }
    // After epoch advances, only latest epoch edges (+ any epoch-0) remain
    // per prune_type_dep_graph_epoch. Cap also bounds single-bucket growth.
    const auto edges = tc.type_dep_graph_edge_count();
    CHECK(edges <= kTypeDepBucketCap, "AC2: edge count ≤ kTypeDepBucketCap");
    CHECK(edges < 50 * 8, "AC2: not O(session length) unbounded");
    // Cap path: flood one bucket without epoch advance.
    TypeRegistry reg2;
    TypeChecker tc2(reg2);
    tc2.set_cache_epoch(1);
    for (std::size_t i = 0; i < kTypeDepBucketCap + 64; ++i)
        tc2.record_type_dependency(99, static_cast<NodeId>(i + 1));
    CHECK(tc2.type_dep_graph_edge_count() <= kTypeDepBucketCap,
          "AC2: cap keeps single-bucket ≤ 256");
}

// ── AC3: same epoch / empty invalidate zero extra stale drop ──
static void ac3_zero_cost_paths() {
    std::println("\n--- AC3: same epoch + empty invalidate → no extra work ---");
    TypeRegistry reg;
    TypeChecker tc(reg);
    tc.set_cache_epoch(5);
    tc.record_type_dependency(1, 1);
    const auto drop0 = g_type_dep_graph_stale_drop_total.load();
    const auto edges0 = tc.type_dep_graph_edge_count();
    tc.set_cache_epoch(5); // same → no prune
    CHECK(g_type_dep_graph_stale_drop_total.load() == drop0, "AC3: same epoch no stale drop");
    CHECK(tc.type_dep_graph_edge_count() == edges0, "AC3: edges unchanged");
    std::vector<NodeId> empty;
    CHECK(tc.invalidate_type_dep_for_nodes(empty) == 0, "AC3: empty invalidate → 0");
    // min_epoch 0 prune is no-op
    CHECK(tc.prune_type_dep_graph_epoch(0) == 0, "AC3: prune_epoch(0) no-op");
}

// ── AC4: query schema ──
static void ac4_query_schema() {
    std::println("\n--- AC4: query:type-dep-partial-merge-stats schema-2355 ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2355") == 2355, "AC4: schema-2355");
    CHECK(href(cs, "issue-2355") == 2355, "AC4: issue-2355");
    CHECK(href(cs, "type-dep-epoch-wired") == 1, "AC4: wired");
    CHECK(href(cs, "type-dep-stale-drop-total") >= 0, "AC4: stale-drop-total");
    CHECK(href(cs, "type-dep-invalidate-total") >= 0, "AC4: invalidate-total");
    CHECK(href(cs, "type-dep-size") >= 0, "AC4: type-dep-size");
    // Lineage retained
    CHECK(href(cs, "schema-2320") == 2320, "AC4: schema-2320 retained");
    CHECK(href(cs, "schema-2283") == 2283 || href(cs, "type-dep-partial-merge-total") >= 0,
          "AC4: #2283 keys retained");
}

// ── AC5: source-cite + invalidate unit ──
static void ac5_source_cite_and_invalidate() {
    std::println("\n--- AC5: source-cite + invalidate unit ---");
    const auto tc = read_file("src/compiler/type_checker.ixx");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto met = read_file("src/compiler/observability_metrics.h");
    CHECK(tc.find("TypeDepEdge") != std::string::npos, "AC5: TypeDepEdge");
    CHECK(tc.find("prune_type_dep_graph_epoch") != std::string::npos, "AC5: epoch prune");
    CHECK(tc.find("invalidate_type_dep_for_nodes") != std::string::npos, "AC5: invalidate");
    CHECK(tc.find("kTypeDepBucketCap") != std::string::npos, "AC5: bucket cap");
    CHECK(tc.find("Issue #2355") != std::string::npos, "AC5: type_checker cites #2355");
    CHECK(tci.find("invalidate_type_dep_for_nodes") != std::string::npos,
          "AC5: infer_flat_partial wires invalidate");
    CHECK(tci.find("Issue #2355") != std::string::npos, "AC5: impl cites #2355");
    CHECK(q.find("schema-2355") != std::string::npos, "AC5: query schema-2355");
    CHECK(q.find("type-dep-stale-drop-total") != std::string::npos, "AC5: query stale-drop");
    CHECK(met.find("type_dep_graph_stale_drop_total") != std::string::npos, "AC5: metrics field");

    TypeRegistry reg;
    TypeChecker tcx(reg);
    tcx.set_cache_epoch(3);
    tcx.record_type_dependency(5, 1);
    tcx.record_type_dependency(5, 2);
    tcx.record_type_dependency(5, 3);
    NodeId dirty[] = {2};
    CHECK(tcx.invalidate_type_dep_for_nodes(dirty) == 1, "AC5: invalidate removes 1");
    auto left = tcx.affected_nodes_for_type(5);
    CHECK(left.size() == 2, "AC5: two edges remain");
    CHECK((left[0] == 1 || left[0] == 3) && (left[1] == 1 || left[1] == 3),
          "AC5: remaining nids are 1 and 3");
}

} // namespace

int run_test_type_dep_epoch_prune_2355() {
    std::println("=== Issue #2355: type_dep epoch prune + NodeId invalidation ===");
    ac5_source_cite_and_invalidate();
    ac1_epoch_prune();
    ac2_session_bound();
    ac3_zero_cost_paths();
    ac4_query_schema();
    std::println("\n=== #2355: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_type_dep_epoch_prune_2355();
}
#endif
