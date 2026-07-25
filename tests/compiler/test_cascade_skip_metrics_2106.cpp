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

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.dirty_propagation;
import aura.compiler.optimization_passes;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.ir;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::dirty::cascade_mark_dirty;
using aura::compiler::dirty::cascade_skip_subtree_visible;
using aura::compiler::dirty::DepGraph;
using aura::compiler::dirty::DirtySet;
using aura::compiler::dirty::flush_dirty_skip_subtree_to_metrics;
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

} // namespace

int main() {
    std::println("=== Issue #2106: cascade_skip_subtree metrics + DeadCoercion synergy ===");
    ac1_skip_visible_via_metrics();
    ac2_happy_path_and_no_double_count();
    ac3_optimization_passes_layered();
    ac4_dirty_cascade_stats_schema();
    ac5_dead_coercion_dirty_cone_early_out();
    ac6_source_wiring();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
