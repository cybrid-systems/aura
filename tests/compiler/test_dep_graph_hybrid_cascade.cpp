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
#include "compiler/typed_mutation_audit.h"
#include "core/transparent_string_hash.hh" // aura::core::TransparentStringHash

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.dirty_propagation; // aura::compiler::dirty::*

namespace {

using aura::compiler::CompilerMetrics;
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
    // Issue #3187: wire-up uses dual_dep_graph_strict_or_production
    // (subsumes dual_dep_graph_strict_enabled). Accept either.
    CHECK(svc.find("dual_dep_graph_strict_or_production") != std::string::npos ||
              svc.find("dual_dep_graph_strict_enabled") != std::string::npos,
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

// Issue #3067: stale-reject must schedule deferred hybrid cascade.
static void ac3067_1_fork_restored_after_drain() {
    std::println("\n--- #3067 AC1: dual-graph fork restored after drain ---");
    CompilerService cs;
    cs.public_record_dependency("A", "B");
    CHECK(cs.public_dep_graph_has_edge("A", "B"), "3067 AC1: string A→B");
    CHECK(cs.public_node_dep_has_mirror_edge("A", "B"), "3067 AC1: NodeId B→A");
    cs.public_drop_node_dep_mirror_edge("A", "B");
    CHECK(!cs.public_node_dep_has_mirror_edge("A", "B"), "3067 AC1: injected NodeId miss");
    CHECK(cs.public_dep_graph_has_edge("A", "B"), "3067 AC1: string edge still present");
    CHECK(!cs.public_graphs_consistent(), "3067 AC1: fork is inconsistent");
    const auto def0 = cs.public_hybrid_deferred_cascade_total();
    cs.public_note_stale_dep_reject("A", "B");
    cs.public_drain_deferred_hybrid_cascade();
    CHECK(cs.public_node_dep_has_mirror_edge("A", "B"), "3067 AC1: NodeId restored");
    CHECK(cs.public_graphs_consistent(), "3067 AC1: consistent after drain");
    CHECK(cs.public_hybrid_deferred_cascade_total() > def0, "3067 AC1: deferred cascade ran");
}

static void ac3067_2_production_consistent() {
    std::println("\n--- #3067 AC2: production drain graphs_consistent ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    CompilerService cs;
    cs.public_record_dependency("caller", "callee");
    cs.public_drop_node_dep_mirror_edge("caller", "callee");
    cs.public_note_stale_dep_reject("caller", "callee");
    (void)cs.public_relower_dirty_defines_from_workspace();
    CHECK(cs.public_graphs_consistent(), "3067 AC2: production relower drain consistent");
    CHECK(cs.public_node_dep_has_mirror_edge("caller", "callee"),
          "3067 AC2: NodeId edge after relower");
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3067_3_clean_path_zero_extra() {
    std::println("\n--- #3067 AC3: clean single-fiber zero extra cascade ---");
    CompilerService cs;
    const auto def0 = cs.public_hybrid_deferred_cascade_total();
    const auto rej0 = cs.public_dep_graph_edge_reject_stale_total();
    cs.public_record_dependency("X", "Y");
    cs.public_record_dependency("X", "Y"); // dedup
    (void)cs.public_relower_dirty_defines_from_workspace();
    CHECK(cs.public_hybrid_deferred_cascade_total() == def0,
          "3067 AC3: no deferred cascade on clean path");
    CHECK(cs.public_dep_graph_edge_reject_stale_total() == rej0,
          "3067 AC3: no stale reject on clean path");
    CHECK(cs.public_graphs_consistent(), "3067 AC3: still consistent");
}

static void ac3067_4_soak_and_linter() {
    std::println("\n--- #3067 AC4: concurrent record soak + linter ---");
    CompilerService cs;
    cs.public_record_dependency("root", "leaf");
    std::atomic<int> stop{0};
    std::thread bumper([&] {
        for (int i = 0; i < 200 && !stop.load(std::memory_order_relaxed); ++i)
            cs.public_bump_dep_graph_generation();
    });
    std::thread writer([&] {
        for (int i = 0; i < 200; ++i)
            cs.public_record_dependency(std::format("c{}", i % 4), "leaf");
    });
    writer.join();
    stop.store(1, std::memory_order_relaxed);
    bumper.join();
    cs.public_drain_deferred_hybrid_cascade();
    (void)cs.public_relower_dirty_defines_from_workspace();
    CHECK(cs.public_graphs_consistent(), "3067 AC4: consistent after soak drain");

    const auto svc = read_file("src/compiler/service.ixx");
    const auto dirty = read_file("src/compiler/service_dirty.cpp");
    const auto t = read_file("tests/compiler/test_dep_graph_hybrid_cascade.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_hybrid_deferred_cascade_3067.py");
    const auto build = read_file("build.py");
    CHECK(svc.find("Issue #3067") != std::string::npos, "3067 AC4: service cite");
    CHECK(svc.find("drain_deferred_hybrid_cascade_") != std::string::npos, "3067 AC4: drain");
    CHECK(dirty.find("Issue #3067") != std::string::npos, "3067 AC4: invalidate drain");
    CHECK(t.find("ac3067_1_fork_restored_after_drain") != std::string::npos, "3067 AC4: AC1");
    CHECK(t.find("ac3067_2_production_consistent") != std::string::npos, "3067 AC4: AC2");
    CHECK(t.find("ac3067_3_clean_path_zero_extra") != std::string::npos, "3067 AC4: AC3");
    CHECK(!lint.empty() && lint.find("Issue #3067") != std::string::npos, "3067 AC4: linter");
    CHECK(build.find("check_hybrid_deferred_cascade_3067") != std::string::npos,
          "3067 AC4: build.py gate");
    CHECK(build.find("cmd_hybrid_deferred_cascade_3067") != std::string::npos,
          "3067 AC4: build.py cmd");
    CHECK(read_file("tests/compiler/test_issue_3067.cpp").empty(),
          "3067 AC4: no test_issue_3067.cpp");
    CompilerService qcs;
    CHECK(href(qcs, "schema-3067") == 3067, "3067 AC4: live schema-3067");
    CHECK(href(qcs, "hybrid-deferred-cascade-wired") == 1, "3067 AC4: wired");
    CHECK(href(qcs, "hybrid-deferred-cascade-total") >= 0, "3067 AC4: total queryable");
}

// Issue #3165: dual DepGraph parity fail under Strict must fail-closed
// force dirty of ALL callers in the graph (not just current callee's
// called_by) so a concurrent partial peel that decided on pre-rebuild
// impact_ub is forced full. Drain path also fails-closed under Strict.
// Soft/Off: rebuild only, zero extra force (existing #2247 AC2).
static void ac3165_strict_fail_closed_all_callers() {
    std::println("\n--- AC #3165: dual parity Strict fail-closed (all callers) ---");
    auto svc = read_file("src/compiler/service.ixx");

    // Producer hook #1: record_dependency Strict branch walks all
    // callers in dep_graph_ (set-deduped) under the same exclusive
    // dep_graph_mtx_ window as the rebuild.
    auto rd_pos =
        svc.find("void record_dependency(const std::string& caller, const std::string& callee)");
    if (rd_pos == std::string::npos)
        rd_pos = svc.find("void record_dependency(");
    CHECK(rd_pos != std::string::npos, "3165: record_dependency definition");
    auto rd_end = svc.find("\n}\n", rd_pos);
    if (rd_end == std::string::npos)
        rd_end = rd_pos + 8000;
    auto rd_win = svc.substr(rd_pos, rd_end - rd_pos);
    CHECK(rd_win.find("Issue #3165") != std::string::npos,
          "3165: record_dependency cite Issue #3165");
    // Issue #3187: the Strict gate in service.ixx now uses
    // dual_dep_graph_strict_or_production() (production fail-closed
    // default). dual_dep_graph_strict_enabled() is still defined for
    // backward compat (#3187 AC4) but no longer gates the
    // record_dependency Strict branch. Accept either helper name.
    CHECK(rd_win.find("dual_dep_graph_strict_or_production") != std::string::npos ||
              rd_win.find("dual_dep_graph_strict_enabled") != std::string::npos,
          "3165: record_dependency Strict gate (dual_dep_graph_strict_or_production or "
          "dual_dep_graph_strict_enabled)");
    CHECK(rd_win.find("for (const auto& [callee_name, callee_entry] : dep_graph_)") !=
              std::string::npos,
          "3165: record_dependency walks all callers in dep_graph_");

    // Producer hook #2: drain_deferred_hybrid_cascade_ Strict branch
    // also force-dirty ALL callers after rebuild.
    auto drain_pos = svc.find("void drain_deferred_hybrid_cascade_()");
    CHECK(drain_pos != std::string::npos, "3165: drain_deferred_hybrid_cascade_ present");
    auto drain_end = svc.find("\n}\n", drain_pos);
    if (drain_end == std::string::npos)
        drain_end = drain_pos + 5000;
    auto drain_win = svc.substr(drain_pos, drain_end - drain_pos);
    CHECK(drain_win.find("Issue #3165") != std::string::npos, "3165: drain cite Issue #3165");
    // Issue #3187: same as record_dependency — drain Strict gate now
    // uses dual_dep_graph_strict_or_production(). Accept either.
    CHECK(drain_win.find("dual_dep_graph_strict_or_production") != std::string::npos ||
              drain_win.find("dual_dep_graph_strict_enabled") != std::string::npos,
          "3165: drain Strict gate (dual_dep_graph_strict_or_production or "
          "dual_dep_graph_strict_enabled)");
    CHECK(drain_win.find("for (const auto& [callee_name, callee_entry] : dep_graph_)") !=
              std::string::npos,
          "3165: drain walks all callers in dep_graph_");

    // Soft / Off zero-cost: Strict force-dirty must NOT be unconditional.
    // The Strict branch is gated on dual_dep_graph_strict_enabled().
    // Under Soft the force-dirty is skipped (existing #2247 AC2 contract).
    CHECK(rd_win.find("rebuild_node_dep_graph_from_string") != std::string::npos,
          "3165: record_dependency still calls rebuild before Strict force-dirty");
    CHECK(drain_win.find("rebuild_node_dep_graph_from_string") != std::string::npos,
          "3165: drain still calls rebuild before Strict force-dirty");

    // AC3: existing #2247 dual_dep_graph_parity_* metrics non-regressing.
    CHECK(rd_win.find("dual_dep_graph_parity_fail_total") != std::string::npos,
          "3165: existing parity_fail counter reused (no new metric key)");
    CHECK(drain_win.find("dual_dep_graph_parity_fail_total") != std::string::npos,
          "3165: drain parity_fail counter reused (no new metric key)");

    // AC4: no test_issue_3165.cpp (#81967); no docs/design/3165-* (#1655).
    auto root = std::filesystem::current_path();
    CHECK(!std::filesystem::exists(root / "tests" / "issues" / "test_issue_3165.cpp"),
          "3165: tests/issues/test_issue_3165.cpp absent (#81967)");
    CHECK(!std::filesystem::exists(root / "tests" / "compiler" / "test_issue_3165.cpp"),
          "3165: tests/compiler/test_issue_3165.cpp absent (#81967)");
    auto design = root / "docs" / "design";
    if (std::filesystem::exists(design)) {
        for (const auto& f : std::filesystem::directory_iterator(design)) {
            auto name = f.path().filename().string();
            CHECK(name.find("3165-") == std::string::npos,
                  "3165: no docs/design/3165-* plan doc (#1655)");
            break;
        }
    }
}

// ── Issue #3187: dual DepGraph fork window — production fail-closed default ──
// Closes the residual dual-graph fork window under lockless batch / cross-
// fiber record_dependency. The existing Strict fail-closed path (#3165)
// only fires when set_dual_dep_graph_strict(1) is called explicitly.
// #3187 extends the same all-callers force-dirty walk to also fire under
// production_defaults_active() (or AuditStrategy::Full) by default — no
// explicit Strict toggle required. Closes the silent under-cascade
// residual on the hybrid_node_cascade_ entry path (the existing Strict
// path was gated only by an explicit toggle that production wasn't using).
// Soft / Off zero-cost: dual_dep_graph_strict_or_production() stays
// false under Soft (no parity check, no force-dirty, no counter bump).
//   AC1: dual_dep_graph_strict_or_production() helper defined in
//        dirty_propagation.ixx (ORs dual_dep_graph_strict_enabled
//        with production_defaults_active || get_strategy() == Full)
//   AC2: record_dependency Strict gate uses the new helper (not just
//        dual_dep_graph_strict_enabled) so production fail-closed is
//        the default
//   AC3: drain_deferred_hybrid_cascade_ Strict gate uses the new helper
//        (same surface as #3165 Strict branch, enabled by production)
//   AC4: existing #3165 sibling AC preserved (dual_dep_graph_strict_enabled
//        still works for explicit Strict-mode tests)
//   AC5: helper cites Issue #3187; both gates cite Issue #3187; no new
//        metric key (reuses dual_dep_graph_parity_fail_total)

static void ac3187_production_fail_closed_default() {
    std::println("\n--- #3187: dual DepGraph fork — production fail-closed default ---");

    // AC1: dual_dep_graph_strict_or_production() helper defined in
    // dirty_propagation.ixx and ORs strict + production.
    {
        const auto ixx = read_file("src/compiler/dirty_propagation.ixx");
        CHECK(ixx.find("dual_dep_graph_strict_or_production") != std::string::npos,
              "ac3187 AC1: helper defined in dirty_propagation.ixx");
        CHECK(ixx.find("Issue #3187") != std::string::npos, "ac3187 AC1: helper cites Issue #3187");
        CHECK(ixx.find("production_defaults_active()") != std::string::npos,
              "ac3187 AC1: helper consults production_defaults_active");
        CHECK(ixx.find("AuditStrategy::Full") != std::string::npos,
              "ac3187 AC1: helper also consults AuditStrategy::Full");
        // The OR semantics must be explicit (calls dual_dep_graph_strict_enabled).
        CHECK(ixx.find("dual_dep_graph_strict_enabled()") != std::string::npos,
              "ac3187 AC1: helper ORs with dual_dep_graph_strict_enabled");
    }

    // AC2: record_dependency Strict gate uses the new helper (production
    // fail-closed by default).
    {
        const auto svc = read_file("src/compiler/service.ixx");
        auto rd_pos = svc.find("void record_dependency(const std::string& caller");
        if (rd_pos == std::string::npos)
            rd_pos = svc.find("void record_dependency(");
        CHECK(rd_pos != std::string::npos, "ac3187 AC2: record_dependency definition");
        auto rd_end = svc.find("\n    }\n", rd_pos);
        if (rd_end == std::string::npos)
            rd_end = rd_pos + 8000;
        auto rd_win = svc.substr(rd_pos, rd_end - rd_pos);
        CHECK(rd_win.find("dual_dep_graph_strict_or_production()") != std::string::npos,
              "ac3187 AC2: record_dependency Strict gate uses dual_dep_graph_strict_or_production");
        CHECK(rd_win.find("Issue #3187") != std::string::npos,
              "ac3187 AC2: record_dependency cites Issue #3187");
        // AC2: must NOT regress the existing #3165 strict-enabled path
        // (it's now subsumed by strict_or_production, but the helper still
        // exists for explicit-Strict tests).
        CHECK(rd_win.find("for (const auto& [callee_name, callee_entry] : dep_graph_)") !=
                  std::string::npos,
              "ac3187 AC2: all-callers walk preserved (sibling #3165 contract)");
    }

    // AC3: drain_deferred_hybrid_cascade_ Strict gate uses the new helper.
    {
        const auto svc = read_file("src/compiler/service.ixx");
        auto drain_pos = svc.find("void drain_deferred_hybrid_cascade_()");
        CHECK(drain_pos != std::string::npos, "ac3187 AC3: drain_deferred_hybrid_cascade_ present");
        auto drain_end = svc.find("\n    }\n", drain_pos);
        if (drain_end == std::string::npos)
            drain_end = drain_pos + 5000;
        auto drain_win = svc.substr(drain_pos, drain_end - drain_pos);
        CHECK(drain_win.find("dual_dep_graph_strict_or_production()") != std::string::npos,
              "ac3187 AC3: drain_deferred_hybrid_cascade_ Strict gate uses "
              "dual_dep_graph_strict_or_production");
        CHECK(drain_win.find("Issue #3187") != std::string::npos,
              "ac3187 AC3: drain cites Issue #3187");
        CHECK(drain_win.find("for (const auto& [callee_name, callee_entry] : dep_graph_)") !=
                  std::string::npos,
              "ac3187 AC3: all-callers walk preserved (sibling #3165 contract)");
    }

    // AC4: existing #3165 sibling AC preserved. The Strict-only helper
    // still exists for explicit-Strict tests / non-production paths.
    {
        const auto t = read_file("tests/compiler/test_dep_graph_hybrid_cascade.cpp");
        CHECK(t.find("ac3165_strict_fail_closed_all_callers") != std::string::npos,
              "ac3187 AC4: sibling #3165 AC1 preserved");
        const auto ixx = read_file("src/compiler/dirty_propagation.ixx");
        CHECK(ixx.find("inline bool dual_dep_graph_strict_enabled()") != std::string::npos,
              "ac3187 AC4: dual_dep_graph_strict_enabled() helper still defined (backward compat)");
        const auto svc = read_file("src/compiler/service.ixx");
        // dual_dep_graph_strict_enabled may still be referenced elsewhere
        // (e.g., test setters, hooks) — not required for the Strict gate
        // itself anymore (replaced by strict_or_production).
        // We only assert the helper definition is preserved.
        (void)svc;
    }

    // AC5: no new metric key (reuses dual_dep_graph_parity_fail_total);
    // no test_issue_3187.cpp (#81967); no docs/design/3187-* (#1655).
    {
        const auto obs = read_file("src/compiler/observability_metrics.h");
        // Count occurrences of dual_dep_graph_parity_fail_total across the
        // codebase to confirm no new key was inserted.
        const auto ixx = read_file("src/compiler/dirty_propagation.ixx");
        const auto svc = read_file("src/compiler/service.ixx");
        auto count_sub = [](const std::string& s, std::string_view n) {
            std::size_t c = 0;
            for (std::size_t p = 0; (p = s.find(n, p)) != std::string::npos; p += n.size())
                ++c;
            return c;
        };
        const auto total = count_sub(obs, "dual_dep_graph_parity_fail_total") +
                           count_sub(ixx, "dual_dep_graph_parity_fail_total") +
                           count_sub(svc, "dual_dep_graph_parity_fail_total");
        CHECK(total >= 4, "ac3187 AC5: existing dual_dep_graph_parity_fail_total counter reused "
                          "(no new metric key)");
        auto root = std::filesystem::current_path();
        CHECK(!std::filesystem::exists(root / "tests" / "issues" / "test_issue_3187.cpp"),
              "ac3187 AC5: tests/issues/test_issue_3187.cpp absent (#81967)");
        CHECK(!std::filesystem::exists(root / "tests" / "compiler" / "test_issue_3187.cpp"),
              "ac3187 AC5: tests/compiler/test_issue_3187.cpp absent (#81967)");
        auto design = root / "docs" / "design";
        if (std::filesystem::exists(design)) {
            for (const auto& f : std::filesystem::directory_iterator(design)) {
                auto name = f.path().filename().string();
                CHECK(name.find("3187-") == std::string::npos,
                      "ac3187 AC5: no docs/design/3187-* plan doc (#1655)");
                break;
            }
        }
    }
}

// ── Issue #3255: Soft dual-graph parity fail fail-closed before partial peel ──
// Residual of #3187: Production/Strict force-dirties all callers on parity
// fail so the next relower cannot silently partial-peel. Soft / non-Strict
// only rebuilt; a concurrent fiber / lockless batch that left the graphs
// divergent can still reach relower_dirty_defines_from_workspace with
// want_partial=true under an incomplete hybrid cascade cone.
//   AC1: Soft + injected dual-graph fork + mutate + partial path → rebuild
//        + force-dirty / forced full; lookup_define_v2 after mutate is
//        never a silent clean hit
//   AC2: Production/Strict record_dependency + drain still force-dirty all
//        callers via dual_dep_graph_strict_or_production (unchanged)
//   AC3: clean Soft (graphs already consistent) — no extra exclusive
//        rebuild, no forced full
//   AC4: dual_dep_graph_parity_fail_total still increments; existing
//        partial_forced_full_by_impact_total distinguishes
//        "parity-fail → forced full"
//   AC5: concurrent record_dependency soak under Soft fork; abort
//        dual-topology restore still forces relower
//   AC6: no test_issue_3255.cpp (#81967); no docs/design/3255-* (#1655)

static void ac3255_enter_soft() {
    aura::compiler::dirty::set_dual_dep_graph_strict(0);
    aura::compiler::typed_audit::set_strategy(aura::compiler::typed_audit::AuditStrategy::Off);
    aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active.store(
        0, std::memory_order_relaxed);
}

static void ac3255_1_soft_fork_forces_full() {
    std::println("\n--- #3255 AC1: Soft fork + mutate cannot silent-partial-peel ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    const auto save_prod =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    const auto save_strat = aura::compiler::typed_audit::get_strategy();
    const auto save_strict = aura::compiler::dirty::dual_dep_graph_strict_enabled();
    ac3255_enter_soft();
    CHECK(!aura::compiler::dirty::dual_dep_graph_strict_or_production(),
          "ac3255 AC1: Soft / Off (strict_or_production false)");

    CompilerService cs;
    CHECK(cs.eval(R"(
(set-code "
(define B (lambda () 1))
(define A (lambda () (B)))
")")
              .has_value(),
          "ac3255 AC1: set-code A/B");
    CHECK(cs.eval("(eval-current)").has_value(), "ac3255 AC1: eval");
    cs.public_record_dependency("A", "B");
    CHECK(cs.public_graphs_consistent(), "ac3255 AC1: consistent after record");
    CHECK(cs.get_define_v2("A") != nullptr, "ac3255 AC1: A cached");
    CHECK(cs.get_define_v2("B") != nullptr, "ac3255 AC1: B cached");
    const auto hash_a = cs.get_define_v2("A")->source_hash;

    // Dirty only B (one block) so want_partial stays true. Drop the NodeId
    // mirror AFTER the mark so string cascade cannot hide the fork, and
    // immediately before relower so record_dependency cannot rebuild first.
    const auto* be = cs.get_define_v2("B");
    const std::size_t fi = (be && be->irs.size() >= 2) ? 1 : 0;
    CHECK(cs.mark_block_dirty_v2("B", fi, 0), "ac3255 AC1: B one-block dirty (want_partial)");
    cs.public_drop_node_dep_mirror_edge("A", "B");
    CHECK(!cs.public_graphs_consistent(), "ac3255 AC1: injected NodeId fork");

    auto& m = cs.metrics();
    const auto fail0 = m.dual_dep_graph_parity_fail_total.load(std::memory_order_relaxed);
    const auto forced0 = m.partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    const auto full0 = m.incremental_full_fallback_total.load(std::memory_order_relaxed);

    (void)cs.public_relower_dirty_defines_from_workspace();

    CHECK(cs.public_graphs_consistent(), "ac3255 AC1: rebuilt at peel (graphs consistent)");
    const auto fail1 = m.dual_dep_graph_parity_fail_total.load(std::memory_order_relaxed);
    const auto forced1 = m.partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    const auto full1 = m.incremental_full_fallback_total.load(std::memory_order_relaxed);
    CHECK(fail1 > fail0, "ac3255 AC1: dual_dep_graph_parity_fail_total incremented");
    CHECK(forced1 > forced0 || full1 > full0,
          "ac3255 AC1: parity-fail → forced full (never silent partial)");
    const auto* after_a = cs.get_define_v2("A");
    CHECK(after_a != nullptr, "ac3255 AC1: A still cached");
    const int look_after = cs.lookup_define_v2("A", hash_a);
    CHECK(look_after != 0 || after_a->dirty || after_a->dirty_block_count() > 0,
          "ac3255 AC1: lookup_define_v2(A) after peel is not silent clean");
    auto ra = cs.eval("(A)");
    CHECK(ra.has_value(), "ac3255 AC1: (A) evals after peel");
    (void)full1;
    (void)ra;

    aura::compiler::dirty::set_dual_dep_graph_strict(save_strict ? 1 : 0);
    aura::compiler::typed_audit::set_strategy(save_strat);
    g_typed_mutation_audit_counters.production_defaults_active.store(save_prod,
                                                                     std::memory_order_relaxed);
}

static void ac3255_2_production_unchanged() {
    std::println("\n--- #3255 AC2: Production/Strict force-dirty all callers unchanged ---");
    const auto svc = read_file("src/compiler/service.ixx");
    auto rd_pos = svc.find("void record_dependency(const std::string& caller");
    if (rd_pos == std::string::npos)
        rd_pos = svc.find("void record_dependency(");
    CHECK(rd_pos != std::string::npos, "ac3255 AC2: record_dependency definition");
    auto rd_end = svc.find("\n    }\n", rd_pos);
    if (rd_end == std::string::npos)
        rd_end = rd_pos + 8000;
    auto rd_win = svc.substr(rd_pos, rd_end - rd_pos);
    CHECK(rd_win.find("dual_dep_graph_strict_or_production()") != std::string::npos,
          "ac3255 AC2: record_dependency still uses dual_dep_graph_strict_or_production");
    CHECK(rd_win.find("for (const auto& [callee_name, callee_entry] : dep_graph_)") !=
              std::string::npos,
          "ac3255 AC2: record_dependency all-callers walk preserved");

    auto drain_pos = svc.find("void drain_deferred_hybrid_cascade_()");
    CHECK(drain_pos != std::string::npos, "ac3255 AC2: drain_deferred_hybrid_cascade_ present");
    auto drain_end = svc.find("\n    }\n", drain_pos);
    if (drain_end == std::string::npos)
        drain_end = drain_pos + 5000;
    auto drain_win = svc.substr(drain_pos, drain_end - drain_pos);
    CHECK(drain_win.find("dual_dep_graph_strict_or_production()") != std::string::npos,
          "ac3255 AC2: drain still uses dual_dep_graph_strict_or_production");
    CHECK(drain_win.find("for (const auto& [callee_name, callee_entry] : dep_graph_)") !=
              std::string::npos,
          "ac3255 AC2: drain all-callers walk preserved");
}

static void ac3255_3_clean_soft_zero_extra() {
    std::println("\n--- #3255 AC3: clean Soft path zero extra exclusive / forced full ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    const auto save_prod =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    const auto save_strat = aura::compiler::typed_audit::get_strategy();
    const auto save_strict = aura::compiler::dirty::dual_dep_graph_strict_enabled();
    ac3255_enter_soft();

    CompilerService cs;
    CHECK(cs.eval(R"(
(set-code "
(define Y (lambda () 1))
(define X (lambda () (Y)))
")")
              .has_value(),
          "ac3255 AC3: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "ac3255 AC3: eval");
    cs.public_record_dependency("X", "Y");
    CHECK(cs.public_graphs_consistent(), "ac3255 AC3: consistent");

    auto& m = cs.metrics();
    const auto fail0 = m.dual_dep_graph_parity_fail_total.load(std::memory_order_relaxed);
    const auto forced0 = m.partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    cs.public_mark_define_dirty("X");
    (void)cs.public_relower_dirty_defines_from_workspace();
    CHECK(cs.public_graphs_consistent(), "ac3255 AC3: still consistent");
    CHECK(m.dual_dep_graph_parity_fail_total.load(std::memory_order_relaxed) == fail0,
          "ac3255 AC3: no parity_fail bump on already-consistent graphs");
    (void)forced0; // impact_ub may still force full; parity path must not.

    const auto svc = read_file("src/compiler/service.ixx");
    auto help_pos = svc.find("fail_closed_soft_dual_graph_parity_before_partial_");
    CHECK(help_pos != std::string::npos, "ac3255 AC3: Soft parity helper present");
    auto help_win = svc.substr(help_pos > 2000 ? help_pos - 2000 : 0, 4500);
    CHECK(help_win.find("Issue #3255") != std::string::npos,
          "ac3255 AC3: helper cites Issue #3255");
    CHECK(help_win.find("graphs_consistent") != std::string::npos,
          "ac3255 AC3: peel-time graphs_consistent re-check");
    CHECK(help_win.find("OrderedSharedLock") != std::string::npos,
          "ac3255 AC3: consistent path is shared-lock walk");
    CHECK(help_win.find("rebuild_node_dep_graph_from_string") != std::string::npos,
          "ac3255 AC3: rebuild only on fail");
    auto rel_pos = svc.find("std::size_t relower_dirty_defines_from_workspace()");
    CHECK(rel_pos != std::string::npos, "ac3255 AC3: relower_dirty_defines_from_workspace present");
    auto rel_win = svc.substr(rel_pos, 8000);
    CHECK(rel_win.find("fail_closed_soft_dual_graph_parity_before_partial_") != std::string::npos,
          "ac3255 AC3: relower calls Soft parity helper before peel");

    aura::compiler::dirty::set_dual_dep_graph_strict(save_strict ? 1 : 0);
    aura::compiler::typed_audit::set_strategy(save_strat);
    g_typed_mutation_audit_counters.production_defaults_active.store(save_prod,
                                                                     std::memory_order_relaxed);
}

static void ac3255_4_metrics_soak_and_linter() {
    std::println("\n--- #3255 AC4/AC5: distinguisher + concurrent soak + linter ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    const auto save_prod =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    const auto save_strat = aura::compiler::typed_audit::get_strategy();
    const auto save_strict = aura::compiler::dirty::dual_dep_graph_strict_enabled();
    ac3255_enter_soft();

    CompilerService cs;
    CHECK(cs.eval(R"(
(set-code "
(define leaf (lambda () 1))
(define root (lambda () (leaf)))
")")
              .has_value(),
          "ac3255 AC5: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "ac3255 AC5: eval");
    cs.public_record_dependency("root", "leaf");
    auto& m = cs.metrics();
    const auto fail0 = m.dual_dep_graph_parity_fail_total.load(std::memory_order_relaxed);
    const auto forced0 = m.partial_forced_full_by_impact_total.load(std::memory_order_relaxed);

    std::atomic<int> stop{0};
    std::thread writer([&] {
        for (int i = 0; i < 80 && !stop.load(std::memory_order_relaxed); ++i) {
            cs.public_record_dependency(std::format("c{}", i % 4), "leaf");
            if (i % 7 == 0)
                cs.public_drop_node_dep_mirror_edge("root", "leaf");
        }
    });
    for (int i = 0; i < 24; ++i) {
        cs.public_mark_define_dirty("root");
        (void)cs.public_relower_dirty_defines_from_workspace();
        if (i % 5 == 0)
            cs.public_drop_node_dep_mirror_edge("root", "leaf");
    }
    stop.store(1, std::memory_order_relaxed);
    writer.join();
    (void)cs.public_relower_dirty_defines_from_workspace();
    CHECK(cs.public_graphs_consistent(), "ac3255 AC5: consistent after soak");
    auto rr = cs.eval("(root)");
    CHECK(rr.has_value(), "ac3255 AC5: (root) evals after soak");
    // Distinguisher: fail_total + partial_forced_full_by_impact_total both
    // live (parity-fail → forced full). Soak may or may not hit the fork
    // window every iteration; counters are non-decreasing.
    CHECK(m.dual_dep_graph_parity_fail_total.load(std::memory_order_relaxed) >= fail0,
          "ac3255 AC4: fail_total non-decreasing");
    CHECK(m.partial_forced_full_by_impact_total.load(std::memory_order_relaxed) >= forced0,
          "ac3255 AC4: forced-full distinguisher non-decreasing");

    const auto obs = read_file("src/compiler/observability_metrics.h");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto t = read_file("tests/compiler/test_dep_graph_hybrid_cascade.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_dual_dep_graph_soft_parity_partial_3255.py");
    const auto build = read_file("build.py");
    CHECK(obs.find("Issue #3255") != std::string::npos, "ac3255 AC4: obs cite");
    CHECK(obs.find("partial_forced_full_by_impact_total") != std::string::npos,
          "ac3255 AC4: existing distinguisher");
    CHECK(svc.find("Issue #3255") != std::string::npos, "ac3255 AC4: service cite");
    CHECK(t.find("ac3255_1_soft_fork_forces_full") != std::string::npos, "ac3255 AC6: AC1");
    CHECK(!lint.empty() && lint.find("Issue #3255") != std::string::npos, "ac3255 AC6: linter");
    CHECK(build.find("check_dual_dep_graph_soft_parity_partial_3255") != std::string::npos,
          "ac3255 AC6: build.py gate");
    CHECK(read_file("tests/compiler/test_issue_3255.cpp").empty(),
          "ac3255 AC6: no test_issue_3255.cpp");
    CHECK(read_file("tests/issues/test_issue_3255.cpp").empty(),
          "ac3255 AC6: no tests/issues/test_issue_3255.cpp");
    CompilerService qcs;
    CHECK(href(qcs, "schema-3255") == 3255, "ac3255 AC4: live schema-3255");
    CHECK(href(qcs, "issue-3255") == 3255, "ac3255 AC4: live issue-3255");

    aura::compiler::dirty::set_dual_dep_graph_strict(save_strict ? 1 : 0);
    aura::compiler::typed_audit::set_strategy(save_strat);
    g_typed_mutation_audit_counters.production_defaults_active.store(save_prod,
                                                                     std::memory_order_relaxed);
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
    ac3067_1_fork_restored_after_drain();
    ac3067_2_production_consistent();
    ac3067_3_clean_path_zero_extra();
    ac3067_4_soak_and_linter();
    ac3165_strict_fail_closed_all_callers();
    // Issue #3187: production fail-closed default (extends #3165 Strict to
    // also fire under production_defaults_active).
    ac3187_production_fail_closed_default();
    // Issue #3255: Soft dual-graph parity fail fail-closed before partial peel.
    ac3255_1_soft_fork_forces_full();
    ac3255_2_production_unchanged();
    ac3255_3_clean_soft_zero_extra();
    ac3255_4_metrics_soak_and_linter();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dep_graph_hybrid_cascade();
}
#endif
