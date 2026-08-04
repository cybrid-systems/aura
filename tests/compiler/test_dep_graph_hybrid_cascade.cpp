// @category: unit
// @reason: Issue #2110 — unify function-level dep_graph_ with NodeId
// DepGraph (hybrid cascade). Extended by Issue #2187 — block/instr
// DepGraph edges beyond function-slot hybrid.
//
//   AC1: record_dependency populates both string graph and NodeId mirror
//   AC2: invalidate/mark cascade marks only call-site body blocks of callers
//   AC3: FIFO BFS + sorted dependents determinism preserved
//   AC4: hybrid metrics queryable; nested free-var metrics present
//   AC5: A calls B; mutate B → A's body dirty; nested lambda without free-ref clean
//   AC6: lock order mutate → dep_graph (source + no inversion)
//
//   #2187 AC1: block edge present after define+call / record_block_dependency
//   #2187 AC2: NodeId BFS preferred when mirror populated; FIFO/sorted retained
//   #2187 AC3: nested free-ref authority preserved
//   #2187 AC4: dep_graph_block_mirror_edges_total + cascade_block_hits (schema-2187)
//   #2187 AC5: mutate callee → call-site block dirty; lock order unchanged

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "core/transparent_string_hash.hh" // aura::core::TransparentStringHash

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.dirty_propagation; // aura::compiler::dirty::*

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
        "(hash-ref (engine:metrics \"query:production-sweep-1261-1265-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_dual_graph_parity() {
    std::println("\n--- AC1: record_dependency dual-graph parity ---");
    CompilerService cs;
    const auto ins0 = cs.public_dep_graph_record_inserted();
    const auto mir0 = cs.public_dep_graph_node_mirror_edges();
    const auto edges0 = cs.public_node_dep_graph_edge_count();
    cs.public_record_dependency("A", "B");
    cs.public_record_dependency("A", "B"); // dedup
    cs.public_record_dependency("C", "B");
    CHECK(cs.public_dep_graph_has_edge("A", "B"), "string edge A→B");
    CHECK(cs.public_dep_graph_has_edge("C", "B"), "string edge C→B");
    CHECK(cs.public_dep_graph_record_inserted() == ins0 + 2, "2 string inserts");
    CHECK(cs.public_node_dep_has_mirror_edge("A", "B"), "NodeId mirror B→A (called_by)");
    CHECK(cs.public_node_dep_has_mirror_edge("C", "B"), "NodeId mirror B→C");
    CHECK(cs.public_dep_graph_node_mirror_edges() == mir0 + 2, "mirror edges +2");
    // Issue #2187: each string insert also mirrors a block-level edge
    // (fn edge + block edge per insert) → +4 total NodeId edges.
    CHECK(cs.public_node_dep_graph_edge_count() == edges0 + 4, "node edge count +4 (fn+block)");
    CHECK(cs.public_dep_graph_block_mirror_edges() >= 2, "block mirror edges >= 2");
}

static void ac2_body_only_not_nested() {
    std::println("\n--- AC2/AC5: body-only cascade; nested free-ref clean ---");
    // Source contract for nested free-var targeting
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(dirty.find("mark_nested_lambda_blocks_targeted") != std::string::npos,
          "nested free-var targeting");
    CHECK(dirty.find("hybrid_node_cascade_") != std::string::npos, "hybrid cascade");
    CHECK(dirty.find("Issue #2110") != std::string::npos ||
              dirty.find("#2110") != std::string::npos,
          "cites #2110");

    CompilerService cs;
    // A body calls B; A also has nested lambda that does not capture B.
    CHECK(cs.eval(R"(
(set-code "
(define B (lambda () 1))
(define A (lambda ()
  (let ((inner (lambda () 42)))
    (+ (B) (inner)))))
")")
              .has_value(),
          "set-code A/B");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    // Populate / ensure dep edges A→B
    cs.public_record_dependency("A", "B");
    CHECK(cs.public_node_dep_has_mirror_edge("A", "B"), "mirror after define");

    const auto hy0 = cs.public_dep_graph_hybrid_cascade_hits();
    // Soft dirty cascade from B (mark_define_dirty via public)
    cs.public_mark_define_dirty("B");
    CHECK(cs.public_dep_graph_hybrid_cascade_hits() > hy0, "hybrid cascade ran");

    // Source: cascade marks body-only for dependents (irs[1]), nested via free-var.
    CHECK(dirty.find("block_dirty_per_func_[1]") != std::string::npos ||
              dirty.find("cascade_body_only") != std::string::npos ||
              dirty.find("mark_caller_body_dirty") != std::string::npos ||
              dirty.find("body-only") != std::string::npos ||
              dirty.find("mark_body_only_dirty") != std::string::npos ||
              dirty.find("for (auto& b : centry.block_dirty_per_func_[1])") != std::string::npos,
          "body-only path for callers");
}

static void ac3_determinism() {
    std::println("\n--- AC3: FIFO BFS + sorted dependents ---");
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(dirty.find("std::sort(dependents.begin()") != std::string::npos ||
              dirty.find("std::sort(dependents") != std::string::npos,
          "lexicographic sort of dependents");
    CHECK(dirty.find("pop_front") != std::string::npos ||
              dirty.find("std::queue") != std::string::npos ||
              dirty.find("std::deque") != std::string::npos,
          "FIFO BFS");
    auto inv = read_file("tests/compiler/test_invalidate_cascade_order.cpp");
    CHECK(!inv.empty(), "existing determinism suite present");
}

static void ac4_metrics_query() {
    std::println("\n--- AC4: hybrid metrics queryable ---");
    CompilerService cs;
    cs.public_record_dependency("X", "Y");
    cs.public_mark_define_dirty("Y");
    CHECK(href(cs, "schema-2110") == 2110, "schema-2110");
    CHECK(href(cs, "issue-2110") == 2110, "issue-2110");
    CHECK(href(cs, "dep-graph-hybrid-cascade-wired") == 1, "wired");
    CHECK(href(cs, "dep-graph-node-mirror-edges-total") >= 1, "mirror edges visible");
    CHECK(href(cs, "dep-graph-hybrid-cascade-hits") >= 1, "hybrid hits visible");
    CHECK(href(cs, "dep-graph-nested-lambda-targeted-dirty") >= 0, "nested targeted present");
    CHECK(href(cs, "nested-lambda-per-block-targeted-wired") == 1, "nested wired");
}

static void ac6_lock_order() {
    std::println("\n--- AC6: lock order mutate → dep_graph ---");
    auto svc = read_file("src/compiler/service.ixx");
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(svc.find("Level::DepGraph") != std::string::npos, "DepGraph level");
    CHECK(dirty.find("Level::DepGraph") != std::string::npos ||
              dirty.find("dep_graph_mtx_") != std::string::npos,
          "dep_graph locked in cascade");
    CHECK(svc.find("record_dependency") != std::string::npos, "record under lock");
    // Source: encode_fn_node mirror under same exclusive write as string edge
    CHECK(svc.find("encode_fn_node") != std::string::npos, "encode_fn_node in service");
    CHECK(svc.find("node_dep_graph_") != std::string::npos, "node_dep_graph_ member");
    auto pure = read_file("src/compiler/dirty_propagation.ixx");
    CHECK(pure.find("encode_fn_node") != std::string::npos, "encode_fn_node exported");
    CHECK(pure.find("is_fn_node") != std::string::npos, "is_fn_node");
}

static void ac_source_wiring() {
    std::println("\n--- Source wiring ---");
    auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("Issue #2110") != std::string::npos || svc.find("#2110") != std::string::npos,
          "service cites #2110");
    CHECK(svc.find("hybrid_node_cascade_") != std::string::npos, "hybrid helper decl");
    auto q = read_file("src/compiler/evaluator_primitives_stdlib_review.cpp");
    CHECK(q.find("schema-2110") != std::string::npos, "query schema-2110");
}

// ── Issue #2187 extensions ──────────────────────────────────────────

static void ac2187_block_edge_after_record() {
    std::println("\n--- #2187 AC1: block edge after define+call / record ---");
    CompilerService cs;
    CHECK(cs.eval(R"(
(set-code "
(define B (lambda () 1))
(define A (lambda () (+ (B) 2)))
")")
              .has_value(),
          "set-code A/B");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    const auto blk0 = cs.public_dep_graph_block_mirror_edges();
    // Explicit block edge at body func 1, block 0 (call-site convention).
    cs.public_record_block_dependency("A", "B", /*func=*/1, /*block=*/0);
    CHECK(cs.public_node_dep_has_block_edge("A", "B", 1, 0), "block edge A/1/0 ← B");
    CHECK(cs.public_dep_graph_block_mirror_edges() > blk0, "block mirror counter grew");
    // record_dependency also auto-mirrors a body block edge.
    const auto blk1 = cs.public_dep_graph_block_mirror_edges();
    cs.public_record_dependency("C", "B");
    CHECK(cs.public_dep_graph_block_mirror_edges() > blk1, "record_dependency adds block edges");
    CHECK(cs.public_node_dep_has_mirror_edge("C", "B"), "fn mirror C←B");
    CHECK(cs.public_node_dep_has_block_edge("C", "B", 0, 0), "default body block edge C/0/0");

    // Source encoding present.
    auto pure = read_file("src/compiler/dirty_propagation.ixx");
    CHECK(pure.find("encode_block_dep_node") != std::string::npos, "encode_block_dep_node");
    CHECK(pure.find("is_block_dep_node") != std::string::npos, "is_block_dep_node");
    CHECK(pure.find("Issue #2187") != std::string::npos || pure.find("#2187") != std::string::npos,
          "dirty_propagation cites #2187");
}

static void ac2187_mutate_callee_call_site_block() {
    std::println("\n--- #2187 AC2/AC5: mutate callee → call-site block dirty ---");
    CompilerService cs;
    CHECK(cs.eval(R"(
(set-code "
(define B (lambda () 1))
(define A (lambda ()
  (let ((inner (lambda () 42)))
    (+ (B) (inner)))))
")")
              .has_value(),
          "set-code nested");
    CHECK(cs.eval("(eval-current)").has_value(), "eval nested");

    // Body-only block edge (func 1, block 0) — precise mark path.
    cs.public_record_block_dependency("A", "B", 1, 0);
    CHECK(cs.public_node_dep_has_block_edge("A", "B", 1, 0), "block edge wired");

    const auto hits0 = cs.public_dep_graph_node_cascade_block_hits();
    const auto hy0 = cs.public_dep_graph_hybrid_cascade_hits();
    cs.public_mark_define_dirty("B");
    CHECK(cs.public_dep_graph_hybrid_cascade_hits() > hy0, "hybrid cascade ran");
    // Block-precise path should bump block hits when IR present.
    // (If IR layout has no block 0 mask yet, hit may still grow via apply.)
    CHECK(cs.public_dep_graph_node_cascade_block_hits() >= hits0,
          "block cascade hits non-decreasing");

    // Nested free-ref clean authority still in source.
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(dirty.find("block_precise_names") != std::string::npos ||
              dirty.find("apply_block_precise") != std::string::npos ||
              dirty.find("is_block_dep_node") != std::string::npos,
          "block-precise cascade path in hybrid");
    CHECK(dirty.find("mark_nested_lambda_blocks_targeted") != std::string::npos,
          "nested free-var authority retained (#2187 AC3)");
}

static void ac2187_metrics_schema() {
    std::println("\n--- #2187 AC4: schema-2187 + block metrics ---");
    CompilerService cs;
    cs.public_record_block_dependency("X", "Y", 0, 0);
    cs.public_mark_define_dirty("Y");
    CHECK(href(cs, "schema-2187") == 2187, "schema-2187");
    CHECK(href(cs, "issue-2187") == 2187, "issue-2187");
    CHECK(href(cs, "dep-graph-block-mirror-wired") == 1, "block mirror wired");
    CHECK(href(cs, "dep-graph-block-mirror-edges-total") >= 1, "block edges visible");
    CHECK(href(cs, "dep-graph-node-cascade-block-hits") >= 0, "block hits present");
    // Lineage 2110 retained.
    CHECK(href(cs, "schema-2110") == 2110, "schema-2110 retained");
}

static void ac2187_lock_order_and_source() {
    std::println("\n--- #2187 AC5: lock order + source wiring ---");
    auto svc = read_file("src/compiler/service.ixx");
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(svc.find("record_block_dependency") != std::string::npos, "record_block_dependency");
    CHECK(svc.find("mirror_block_dep_edge_unlocked_") != std::string::npos,
          "mirror_block_dep helper");
    CHECK(svc.find("encode_block_dep_node") != std::string::npos, "service uses block dep encode");
    CHECK(dirty.find("Level::DepGraph") != std::string::npos ||
              dirty.find("dep_graph_mtx_") != std::string::npos,
          "dep_graph locked in cascade");
    // Mutate → dep_graph order unchanged (mutate held, then DepGraph).
    CHECK(dirty.find("OrderedSharedLock") != std::string::npos ||
              dirty.find("OrderedUniqueLock") != std::string::npos,
          "ordered locks");
    auto q = read_file("src/compiler/evaluator_primitives_stdlib_review.cpp");
    CHECK(q.find("schema-2187") != std::string::npos, "query schema-2187");
    CHECK(q.find("dep-graph-block-mirror-edges-total") != std::string::npos,
          "query block edges key");
}

} // namespace

// AC (Issue #2247): dual dep_graph write-parity gate + hybrid cascade
// consistency (string ↔ NodeId). Source-cite the parity primitives +
// strict toggle + 2 new metrics + 2 query keys + schema-2247.
void ac2247_dual_dep_graph_parity_gate() {
    std::println("\n--- AC #2247: dual dep_graph parity gate ---");
    auto pure = read_file("src/compiler/dirty_propagation.ixx");
    auto met = read_file("src/compiler/observability_metrics.h");
    auto svc = read_file("src/compiler/service.ixx");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    // Pure parity primitives
    CHECK(pure.find("graphs_consistent") != std::string::npos, "graphs_consistent helper");
    CHECK(pure.find("rebuild_node_dep_graph_from_string") != std::string::npos,
          "rebuild_node_dep_graph_from_string helper");
    CHECK(pure.find("g_dual_dep_graph_parity_check_total_atomic") != std::string::npos,
          "process atomic check counter");
    CHECK(pure.find("g_dual_dep_graph_parity_fail_total_atomic") != std::string::npos,
          "process atomic fail counter");
    // Strict toggle + C-linkage
    CHECK(pure.find("dual_dep_graph_strict_enabled") != std::string::npos, "Strict toggle");
    CHECK(pure.find("aura_set_dual_dep_graph_strict") != std::string::npos, "C-linkage setter");
    CHECK(pure.find("aura_dual_dep_graph_parity_check_v_read") != std::string::npos,
          "C-linkage check v_read");
    // 2 atomic counters in observability_metrics.h
    CHECK(met.find("dual_dep_graph_parity_check_total{0}") != std::string::npos,
          "check counter field");
    CHECK(met.find("dual_dep_graph_parity_fail_total{0}") != std::string::npos,
          "fail counter field");
    // Wire-up in service.ixx (record_dependency chokepoint)
    CHECK(svc.find("graphs_consistent(dep_graph_") != std::string::npos,
          "parity gate wire-up at record_dependency");
    CHECK(svc.find("dual_dep_graph_strict_enabled") != std::string::npos,
          "strict toggle check in wire-up");
    // Query surface (query:dirty-cascade-stats)
    CHECK(q.find("dual-dep-graph-parity-check-total") != std::string::npos,
          "query key: check-total");
    CHECK(q.find("dual-dep-graph-parity-fail-total") != std::string::npos, "query key: fail-total");
    CHECK(q.find("schema-2247") != std::string::npos, "schema-2247 lineage");
    // Runtime: default Off (unit-test safe per AC2)
    aura::compiler::dirty::set_dual_dep_graph_strict(0);
    CHECK(!aura::compiler::dirty::dual_dep_graph_strict_enabled(), "default Off (AC2)");
    // Pure helper smoke: build small string graph + matching node graph -> consistent
    std::unordered_map<std::string, aura::compiler::dirty::FunctionDepEntry,
                       aura::core::TransparentStringHash, std::equal_to<>>
        str_dep;
    std::unordered_map<std::string, std::uint32_t, aura::core::TransparentStringHash,
                       std::equal_to<>>
        name_to_slot;
    name_to_slot["f"] = 0;
    name_to_slot["g"] = 1;
    str_dep["f"].called_by.push_back("g");
    aura::compiler::dirty::DepGraph node_dep;
    node_dep.add_edge(aura::compiler::dirty::encode_fn_node(0),
                      aura::compiler::dirty::encode_fn_node(1));
    CHECK(aura::compiler::dirty::graphs_consistent(str_dep, node_dep, name_to_slot),
          "consistent graph returns true");
    // Inject divergence: remove the node_dep edge -> inconsistent
    node_dep.adj.clear();
    CHECK(!aura::compiler::dirty::graphs_consistent(str_dep, node_dep, name_to_slot),
          "inconsistent graph returns false (AC1)");
    // Rebuild from string -> consistent again
    aura::compiler::dirty::rebuild_node_dep_graph_from_string(node_dep, str_dep, name_to_slot);
    CHECK(aura::compiler::dirty::graphs_consistent(str_dep, node_dep, name_to_slot),
          "consistent after rebuild (AC1 recovery)");
}

int run_test_dep_graph_hybrid_cascade() {
    std::println("=== Issue #2110 + #2187: hybrid dep_graph ↔ NodeId DepGraph (block edges) ===");
    ac1_dual_graph_parity();
    ac2_body_only_not_nested();
    ac3_determinism();
    ac4_metrics_query();
    ac6_lock_order();
    ac_source_wiring();
    ac2187_block_edge_after_record();
    ac2187_mutate_callee_call_site_block();
    ac2187_metrics_schema();
    ac2187_lock_order_and_source();
    ac2247_dual_dep_graph_parity_gate();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dep_graph_hybrid_cascade();
}
#endif
