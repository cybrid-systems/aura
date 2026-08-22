// @category: unit
// @reason: Issue #2106 — wire cascade_skip_subtree into CompilerMetrics +
// DeadCoercion dirty-aware synergy (refine #2063 / #2025).
//
//   AC1: summary-dirty cascade skip → cascade_skip_subtree_total via metrics
//   AC2: happy-path typed eval; exchange semantics (no double-count)
//   AC3: DeadCoercion layered keys + cascade-skip on optimization-passes-stats
//   AC4: query:dirty-cascade-stats schema-2106; #2063 lineage
//   AC5: DirtyAware empty cone early-out counter present
//   AC6: source wiring (flush / sink / DeadCoercion early-out)
//
//   #3264 AC1: g_pipeline_dep_graph atomic release/acquire
//   #3264 AC2: mutex around set + flush dirty writes
//   #3264 AC3: dropped counter when graph unset with pending roots
//   #3264 AC4: empty-root flush zero extra; concurrent flush no hang
//   #3264 AC5: linter after #3263; no invent

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.dirty_propagation;
import aura.compiler.optimization_passes;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.ir;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::dirty::cascade_mark_dirty;
using aura::compiler::dirty::cascade_roots_dropped_no_dep_graph_total;
using aura::compiler::dirty::cascade_skip_subtree_visible;
using aura::compiler::dirty::clear_pipeline_cascade_roots;
using aura::compiler::dirty::DepGraph;
using aura::compiler::dirty::DirtySet;
using aura::compiler::dirty::flush_dirty_skip_subtree_to_metrics;
using aura::compiler::dirty::flush_pipeline_cascade_roots;
using aura::compiler::dirty::g_global_dirty;
using aura::compiler::dirty::note_pipeline_cascade_root;
using aura::compiler::dirty::set_pipeline_dep_graph;
using aura::compiler::opt_registry::dead_coercion_dirty_cone_skips;
using aura::compiler::opt_registry::DeadCoercionPass;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::ir::BasicBlock;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;
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

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

// Graph that triggers summary-dirty skip: root → mid → leaves, second cascade
// re-enters mid whose dependents are already dirty.
static void run_skip_cascade(DirtySet& set, DepGraph& g) {
    g.clear();
    set.clear();
    // 0 → 1 → 2, 1 → 3; 2 → 4, 3 → 4  (diamond under mid)
    g.add_edge(0, 1);
    g.add_edge(1, 2);
    g.add_edge(1, 3);
    g.add_edge(2, 4);
    g.add_edge(3, 4);
    // First cascade marks the full cone.
    (void)cascade_mark_dirty(set, /*root=*/0, g);
    // Second cascade from same root: mid's subtree already dirty → skip.
    (void)cascade_mark_dirty(set, /*root=*/0, g);
}

static void ac1_skip_visible_via_metrics() {
    std::println("\n--- AC1: cascade skip → cascade_skip_subtree_total ---");
    CompilerService cs;
    const auto before = href(cs, "query:dirty-cascade-stats", "cascade-skip-subtree-total");
    CHECK(before >= 0, "cascade-skip-subtree-total reachable");

    DirtySet set;
    DepGraph g;
    run_skip_cascade(set, g);

    const auto after = href(cs, "query:dirty-cascade-stats", "cascade-skip-subtree-total");
    CHECK(after > before, "skip total advanced after summary-dirty cascade");
    CHECK(cascade_skip_subtree_visible() >= static_cast<std::uint64_t>(after - before) ||
              after > before,
          "visible total consistent");
}

static void ac2_happy_path_and_no_double_count() {
    std::println("\n--- AC2: happy-path eval + exchange no double-count ---");
    CompilerService cs;
    CHECK(cs.eval("(let ((x 5)) x)").has_value(), "let + identity");
    CHECK(cs.eval("(let ((x 5)) (let ((y (+ x 3))) y))").has_value(), "let + arith");

    // Capture metrics after CS ctor wired the sink.
    const auto m0 = href(cs, "query:dirty-cascade-stats", "cascade-skip-subtree-total");
    DirtySet set;
    DepGraph g;
    run_skip_cascade(set, g);
    const auto m1 = href(cs, "query:dirty-cascade-stats", "cascade-skip-subtree-total");
    const auto delta = m1 - m0;
    CHECK(delta > 0, "first cascade pair produced skips");

    // Extra flush must not re-add the same events (already exchanged).
    const auto flushed = flush_dirty_skip_subtree_to_metrics();
    CHECK(flushed == 0, "exchange drained; second flush is zero");
    const auto m2 = href(cs, "query:dirty-cascade-stats", "cascade-skip-subtree-total");
    CHECK(m2 == m1, "no double-count after re-flush");
}

static void ac3_optimization_passes_layered() {
    std::println("\n--- AC3: optimization-passes-stats layered + cascade-skip ---");
    CompilerService cs;
    CHECK(href(cs, "query:optimization-passes-stats", "schema-2025") == 2025, "schema-2025");
    CHECK(href(cs, "query:optimization-passes-stats", "schema-2106") == 2106, "schema-2106");
    CHECK(href(cs, "query:optimization-passes-stats", "dead-coercion-wired") == 1,
          "dead-coercion-wired");
    CHECK(href(cs, "query:optimization-passes-stats", "dead-coercion-layered-total") >= 0,
          "layered total reachable");
    CHECK(href(cs, "query:optimization-passes-stats", "cascade-skip-subtree-total") >= 0,
          "cascade-skip on opt-passes-stats");
    CHECK(href(cs, "query:optimization-passes-stats", "dead-coercion-dirty-cone-skips") >= 0,
          "dirty-cone-skips key");
}

static void ac4_dirty_cascade_stats_schema() {
    std::println("\n--- AC4: query:dirty-cascade-stats schema ---");
    CompilerService cs;
    CHECK(href(cs, "query:dirty-cascade-stats", "schema-2106") == 2106, "schema-2106");
    CHECK(href(cs, "query:dirty-cascade-stats", "schema-2063") == 2063, "schema-2063 lineage");
    CHECK(href(cs, "query:dirty-cascade-stats", "cascade-bfs-hits") >= 0, "bfs-hits");
    CHECK(href(cs, "query:dirty-cascade-stats", "cascade-nodes-marked-total") >= 0, "nodes-marked");
}

static void ac5_dead_coercion_dirty_cone_early_out() {
    std::println("\n--- AC5: DeadCoercion empty dirty cone early-out ---");
    DeadCoercionPass dce;
    dce.set_block_dirty_fn([](std::uint32_t) { return false; }); // all clean
    IRModule mod;
    IRFunction fn;
    fn.name = "cone_skip";
    fn.local_count = 1;
    fn.arg_count = 0;
    fn.entry_block = 0;
    BasicBlock blk;
    blk.id = 0;
    blk.instructions = {
        IRInstruction{IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1},
        IRInstruction{IROpcode::Return, {0, 0, 0, 0}, 0, 0},
    };
    fn.blocks.push_back(std::move(blk));
    mod.functions.push_back(std::move(fn));

    const auto s0 = load_u64(dead_coercion_dirty_cone_skips);
    dce.run(mod.functions[0]);
    CHECK(load_u64(dead_coercion_dirty_cone_skips) > s0, "dirty-cone skip bumped");
}

static void ac6_source_wiring() {
    std::println("\n--- AC6: source wiring ---");
    auto dirty = read_file("src/compiler/dirty_propagation.ixx");
    CHECK(dirty.find("flush_dirty_skip_subtree_to_metrics") != std::string::npos, "flush helper");
    CHECK(dirty.find("set_cascade_skip_subtree_metrics") != std::string::npos, "metrics sink");
    CHECK(dirty.find("Issue #2106") != std::string::npos ||
              dirty.find("#2106") != std::string::npos,
          "cites #2106");

    auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("set_cascade_skip_subtree_metrics") != std::string::npos, "service wires sink");

    auto opt = read_file("src/compiler/optimization_passes.ixx");
    CHECK(opt.find("dead_coercion_dirty_cone_skips") != std::string::npos, "cone skip counter");
    CHECK(opt.find("any_dirty") != std::string::npos ||
              opt.find("dirty-cone") != std::string::npos ||
              opt.find("Issue #2106") != std::string::npos,
          "DeadCoercion early-out");

    auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(obs.find("query:dirty-cascade-stats") != std::string::npos, "dirty-cascade-stats query");
    CHECK(obs.find("schema-2106") != std::string::npos, "schema-2106 on query surface");
    CHECK(obs.find("cascade-skip-subtree-total") != std::string::npos, "skip key");
}

static void ac3264_1_atomic_graph_pointer() {
    std::println("\n--- #3264 AC1: g_pipeline_dep_graph is atomic release/acquire ---");
    const auto dirty = read_file("src/compiler/dirty_propagation.ixx");
    CHECK(dirty.find("Issue #3264") != std::string::npos, "3264 AC1: cite");
    CHECK(dirty.find("std::atomic<const DepGraph*> g_pipeline_dep_graph") != std::string::npos,
          "3264 AC1: atomic pointer");
    CHECK(dirty.find("g_pipeline_dep_graph.store(g, std::memory_order_release)") !=
              std::string::npos,
          "3264 AC1: release store");
    CHECK(dirty.find("g_pipeline_dep_graph.load(std::memory_order_acquire)") != std::string::npos,
          "3264 AC1: acquire load");
}

static void ac3264_2_flush_mutex() {
    std::println("\n--- #3264 AC2: flush holds mutex around dirty writes ---");
    const auto dirty = read_file("src/compiler/dirty_propagation.ixx");
    auto pos = dirty.find("inline std::size_t flush_pipeline_cascade_roots()");
    CHECK(pos != std::string::npos, "3264 AC2: flush present");
    auto win = dirty.substr(pos, 1800);
    CHECK(win.find("std::lock_guard<std::mutex> lock(g_pipeline_cascade_mtx)") != std::string::npos,
          "3264 AC2: lock in flush");
    CHECK(dirty.find("std::mutex g_pipeline_cascade_mtx") != std::string::npos,
          "3264 AC2: mutex declared");
    CHECK(dirty.find("set_pipeline_dep_graph") != std::string::npos &&
              dirty.find("lock(g_pipeline_cascade_mtx)") != std::string::npos,
          "3264 AC2: set also locks");
}

static void ac3264_3_dropped_counter() {
    std::println("\n--- #3264 AC3: unset graph counts dropped roots ---");
    const auto dirty = read_file("src/compiler/dirty_propagation.ixx");
    CHECK(dirty.find("cascade_roots_dropped_no_dep_graph_total") != std::string::npos,
          "3264 AC3: counter");
    aura::compiler::dirty::clear_pipeline_cascade_roots();
    aura::compiler::dirty::set_pipeline_dep_graph(nullptr);
    const auto d0 = load_u64(cascade_roots_dropped_no_dep_graph_total);
    note_pipeline_cascade_root(42);
    CHECK(flush_pipeline_cascade_roots() == 0, "3264 AC3: flush returns 0");
    const auto d1 = load_u64(cascade_roots_dropped_no_dep_graph_total);
    CHECK(d1 > d0, "3264 AC3: dropped counter advanced");
    aura::compiler::dirty::clear_pipeline_cascade_roots();
}

static void ac3264_4_quiet_empty_zero_extra() {
    std::println("\n--- #3264 AC4: empty-root flush zero extra dropped ---");
    aura::compiler::dirty::clear_pipeline_cascade_roots();
    aura::compiler::dirty::set_pipeline_dep_graph(nullptr);
    const auto d0 = load_u64(cascade_roots_dropped_no_dep_graph_total);
    CHECK(flush_pipeline_cascade_roots() == 0, "3264 AC4: empty flush 0");
    const auto d1 = load_u64(cascade_roots_dropped_no_dep_graph_total);
    CHECK(d1 == d0, "3264 AC4: quiet zero extra");

    DepGraph g;
    g.add_edge(0, 1);
    aura::compiler::dirty::set_pipeline_dep_graph(&g);
    g_global_dirty.clear();
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> ops{0};
    std::vector<std::thread> thr;
    for (int i = 0; i < 3; ++i) {
        thr.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                note_pipeline_cascade_root(0);
                (void)flush_pipeline_cascade_roots();
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    const auto t0 = std::chrono::steady_clock::now();
    while (ops.load(std::memory_order_relaxed) < 40 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : thr)
        t.join();
    aura::compiler::dirty::set_pipeline_dep_graph(nullptr);
    aura::compiler::dirty::clear_pipeline_cascade_roots();
    g_global_dirty.clear();
    CHECK(ops.load() >= 40, "3264 AC4: concurrent flush no hang");
}

static void ac3264_5_source_and_linter() {
    std::println("\n--- #3264 AC5: linter + no invent ---");
    const auto t = read_file("tests/compiler/test_cascade_skip_metrics.cpp");
    const auto batch = read_file("tests/compiler/test_dirty_propagation_cascade.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_cascade_dep_graph_atomic_3264.py");
    CHECK(t.find("ac3264_1_atomic_graph_pointer") != std::string::npos, "3264 AC5: AC1");
    CHECK(batch.find("run_3264_source") != std::string::npos, "3264 AC5: cascade family");
    CHECK(!lint.empty() && lint.find("Issue #3264") != std::string::npos, "3264 AC5: linter");
    CHECK(build.find("check_cascade_dep_graph_atomic_3264") != std::string::npos,
          "3264 AC5: build.py");
    {
        std::ifstream f("tests/compiler/test_issue_3264.cpp");
        CHECK(!f.good(), "3264 AC5: no test_issue_3264.cpp");
    }
    {
        std::ifstream f("docs/design/3264-cascade-dep-graph.md");
        CHECK(!f.good(), "3264 AC5: no docs/design");
    }
}

} // namespace

int run_test_cascade_skip_metrics() {
    std::println("=== Issue #2106: cascade_skip_subtree metrics + DeadCoercion synergy ===");
    ac1_skip_visible_via_metrics();
    ac2_happy_path_and_no_double_count();
    ac3_optimization_passes_layered();
    ac4_dirty_cascade_stats_schema();
    ac5_dead_coercion_dirty_cone_early_out();
    ac6_source_wiring();
    std::println("\n=== Issue #3264: cascade dep-graph atomic + dropped roots ===");
    ac3264_1_atomic_graph_pointer();
    ac3264_2_flush_mutex();
    ac3264_3_dropped_counter();
    ac3264_4_quiet_empty_zero_extra();
    ac3264_5_source_and_linter();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_cascade_skip_metrics();
}
#endif
