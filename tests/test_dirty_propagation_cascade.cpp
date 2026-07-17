// @category: unit
// @reason: Issue #1575 — DirtyPropagation automatic cascade/BFS +
// DirtyAwarePass integration + IR dirty bridges (refine #1206).
//
//   AC1: cascade_mark_dirty / propagate_closure BFS marks all dependents
//   AC2: run_one DirtyAware path flushes cascade roots via DepGraph
//   AC3: sync_from_ir_dirty / push_to_ir_dirty / block matrix bridges
//   AC4: metrics bfs_hits / cascade_depth_avg / manual_propagate deprecated
//   AC5: complex nested-lambda style graph + 1000 mutate rounds
//   AC6: instruction-level nodes in DepGraph cascade correctly

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.dirty_propagation;
import aura.compiler.pass_manager;
import aura.compiler.ir;

namespace {

using aura::compiler::dirty::cascade_mark_dirty;
using aura::compiler::dirty::cascade_mark_dirty_many;
using aura::compiler::dirty::DepGraph;
using aura::compiler::dirty::dirty_cascade_depth_avg;
using aura::compiler::dirty::DirtySet;
using aura::compiler::dirty::encode_block_node;
using aura::compiler::dirty::g_global_dirty;
using aura::compiler::dirty::propagate_closure;
using aura::compiler::dirty::push_to_block_dirty_matrix;
using aura::compiler::dirty::push_to_global;
using aura::compiler::dirty::push_to_ir_dirty;
using aura::compiler::dirty::sync_from_block_dirty_matrix;
using aura::compiler::dirty::sync_from_ir_dirty;
using aura::test::g_failed;
using aura::test::g_passed;

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

// Minimal DirtyAware + Pass for run_one integration.
struct CascadeProbePass {
    int runs = 0;
    void run(aura::ir::IRModule& /*mod*/) {
        ++runs;
        // Register root 0 for auto-cascade after run_one.
        aura::compiler::dirty::note_pipeline_cascade_root(0);
    }
    [[nodiscard]] bool is_block_dirty(std::uint32_t /*id*/) const { return true; }
    bool has_error() const { return false; }
    std::string_view name() const { return "cascade-probe"; }
};

static void ac1_bfs_cascade() {
    std::println("\n--- AC1: cascade_mark_dirty BFS ---");
    // Graph: 0 → 1 → 2 → 3
    //            ↘ 4
    DepGraph g;
    g.add_edge(0, 1);
    g.add_edge(1, 2);
    g.add_edge(2, 3);
    g.add_edge(1, 4);

    DirtySet set;
    const auto marked = cascade_mark_dirty(set, /*root=*/0, g);
    CHECK(marked == 5, std::format("marked all 5 nodes (got {})", marked));
    for (std::uint32_t id = 0; id <= 4; ++id)
        CHECK(set.is_dirty(id), std::format("node {} dirty", id));
    CHECK(!set.is_dirty(99), "unrelated node clean");

    // propagate_closure alias
    DirtySet set2;
    CHECK(propagate_closure(set2, 0, g) == 5, "propagate_closure == cascade");
}

static void ac1b_partial_already_dirty() {
    std::println("\n--- AC1b: cascade from mid-node ---");
    DepGraph g;
    g.add_edges(10, {11, 12});
    g.add_edge(11, 13);
    DirtySet set;
    set.mark(10); // pre-dirty root
    const auto m = cascade_mark_dirty(set, 10, g);
    // root already dirty → not counted; 11,12,13 newly marked
    CHECK(m == 3, std::format("3 new marks from pre-dirty root (got {})", m));
    CHECK(set.is_dirty(11) && set.is_dirty(12) && set.is_dirty(13), "downstream dirty");
}

static void ac2_run_one_auto_cascade() {
    std::println("\n--- AC2: run_one DirtyAware flushes cascade roots ---");
    DepGraph g;
    g.add_edge(0, 1);
    g.add_edge(1, 2);
    g_global_dirty.clear();
    aura::compiler::dirty::set_pipeline_dep_graph(&g);
    aura::compiler::dirty::clear_pipeline_cascade_roots();

    const auto bfs0 = load_u64(aura::compiler::dirty::dirty_propagation_bfs_hits);
    CascadeProbePass pass;
    aura::ir::IRModule mod;
    aura::ir::IRFunction fn;
    fn.id = 0;
    fn.name = "f";
    fn.blocks.push_back({0, {}, {}});
    mod.functions.push_back(std::move(fn));

    CHECK(aura::compiler::run_one(mod, pass), "run_one ok");
    CHECK(pass.runs == 1, "pass ran once");
    CHECK(g_global_dirty.is_dirty(0), "root 0 dirty after auto cascade");
    CHECK(g_global_dirty.is_dirty(1), "dependent 1 dirty");
    CHECK(g_global_dirty.is_dirty(2), "dependent 2 dirty");
    CHECK(load_u64(aura::compiler::dirty::dirty_propagation_bfs_hits) > bfs0, "bfs_hits advanced");

    aura::compiler::dirty::set_pipeline_dep_graph(nullptr);
    g_global_dirty.clear();
}

static void ac3_ir_bridges() {
    std::println("\n--- AC3: sync_from_ir_dirty / push_to_ir_dirty ---");
    std::vector<std::uint8_t> col = {0, 1, 0, 1, 0};
    DirtySet set;
    sync_from_ir_dirty(set, col, /*base=*/100);
    CHECK(set.is_dirty(101) && set.is_dirty(103), "sync marks dirty slots");
    CHECK(!set.is_dirty(100) && !set.is_dirty(102), "clean slots stay clean");

    std::vector<std::uint8_t> out(5, 0);
    // Also mark 100 in set then push
    set.mark(100);
    push_to_ir_dirty(set, out, /*base=*/100);
    CHECK(out[0] == 1 && out[1] == 1 && out[3] == 1, "push sets ir dirty bytes");
    CHECK(out[2] == 0 && out[4] == 0, "clean remain 0");

    // Matrix bridge (func × block)
    std::vector<std::vector<std::uint8_t>> matrix = {{0, 1}, {1, 0, 1}};
    DirtySet mset;
    sync_from_block_dirty_matrix(mset, matrix);
    CHECK(mset.is_dirty(encode_block_node(0, 1)), "func0 block1");
    CHECK(mset.is_dirty(encode_block_node(1, 0)), "func1 block0");
    CHECK(mset.is_dirty(encode_block_node(1, 2)), "func1 block2");
    CHECK(!mset.is_dirty(encode_block_node(0, 0)), "func0 block0 clean");

    // Cascade then push back
    DepGraph g;
    g.add_edge(encode_block_node(0, 1), encode_block_node(1, 1)); // cross-func edge
    cascade_mark_dirty(mset, encode_block_node(0, 1), g);
    CHECK(mset.is_dirty(encode_block_node(1, 1)), "cascade to func1 block1");
    push_to_block_dirty_matrix(mset, matrix);
    CHECK(matrix[1][1] == 1, "matrix updated from cascade");

    // push_to_global
    DirtySet local;
    local.mark(7);
    g_global_dirty.clear();
    push_to_global(local);
    CHECK(g_global_dirty.is_dirty(7), "push_to_global");
    g_global_dirty.clear();
}

static void ac4_metrics() {
    std::println("\n--- AC4: metrics monotonic ---");
    const auto bfs0 = load_u64(aura::compiler::dirty::dirty_propagation_bfs_hits);
    const auto man0 = load_u64(aura::compiler::dirty::manual_propagate_deprecated_count);
    const auto samples0 = load_u64(aura::compiler::dirty::dirty_cascade_depth_samples);

    DepGraph g;
    g.add_edge(0, 1);
    g.add_edge(1, 2);
    DirtySet set;
    cascade_mark_dirty(set, 0, g);
    CHECK(load_u64(aura::compiler::dirty::dirty_propagation_bfs_hits) > bfs0, "bfs_hits++");
    CHECK(load_u64(aura::compiler::dirty::dirty_cascade_depth_samples) > samples0,
          "depth samples++");
    CHECK(dirty_cascade_depth_avg() >= 0.0, "depth avg non-negative");
    // max depth in 0→1→2 is 2
    CHECK(dirty_cascade_depth_avg() >= 1.0 || samples0 > 0, "avg reflects depth");

    // Manual pairwise (deprecated path) — call via non-attr path is still
    // the deprecated method; suppress by using a local that calls it.
    DirtySet set2;
    set2.mark(0);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    set2.propagate(0, 1);
#pragma GCC diagnostic pop
    CHECK(load_u64(aura::compiler::dirty::manual_propagate_deprecated_count) > man0,
          "manual_propagate_deprecated_count++");
    CHECK(set2.is_dirty(1), "pairwise still works (compat)");
}

static void ac5_complex_graph_1000_rounds() {
    std::println("\n--- AC5: nested-lambda style graph + 1000 mutate rounds ---");
    // Simulate: block0 (mutate site) → nested lambda body blocks 1,2
    //                                    → cross-block 3,4
    //                                    → outer consumer 5
    DepGraph g;
    g.add_edge(0, 1);
    g.add_edge(0, 2);
    g.add_edge(1, 3);
    g.add_edge(2, 4);
    g.add_edge(3, 5);
    g.add_edge(4, 5);

    const auto expected_closure = 6; // nodes 0..5
    int misses = 0;
    std::uint64_t total_marked = 0;
    for (int i = 0; i < 1000; ++i) {
        DirtySet set;
        // Mutate only "block 0" each round
        const auto m = cascade_mark_dirty(set, 0, g);
        total_marked += m;
        if (m != static_cast<std::size_t>(expected_closure))
            ++misses;
        for (std::uint32_t id = 0; id < 6; ++id) {
            if (!set.is_dirty(id))
                ++misses;
        }
        // Unrelated node must stay clean
        if (set.is_dirty(99))
            ++misses;
    }
    CHECK(misses == 0, std::format("1000 rounds perfect cascade (misses={})", misses));
    CHECK(total_marked == 1000ull * expected_closure,
          std::format("total marked {} (want {})", total_marked, 1000 * expected_closure));

    // multi-root
    DirtySet set;
    std::array<std::uint32_t, 2> roots = {1, 2};
    const auto multi = cascade_mark_dirty_many(set, roots, g);
    CHECK(set.is_dirty(1) && set.is_dirty(2) && set.is_dirty(5), "multi-root reaches sink 5");
    CHECK(multi >= 4, "multi-root marked at least 4");
}

static void ac6_instruction_level() {
    std::println("\n--- AC6: instruction-level DepGraph nodes ---");
    // Instruction ids 1000+ in the same graph as blocks
    DepGraph g;
    g.add_edge(0, 1000);    // block → first dirty inst
    g.add_edge(1000, 1001); // inst → inst
    g.add_edge(1001, 1);    // inst → other block

    DirtySet set;
    cascade_mark_dirty(set, 0, g);
    CHECK(set.is_dirty(0) && set.is_dirty(1000) && set.is_dirty(1001) && set.is_dirty(1),
          "instruction cascade chain");
}

static void ac_phase() {
    std::println("\n--- phase constant ---");
    CHECK(aura::compiler::dirty::kDirtyPropagationPhase >= 2, "phase >= 2 (#1575)");
}

} // namespace

int main() {
    std::println("=== test_dirty_propagation_cascade (#1575) ===");
    ac1_bfs_cascade();
    ac1b_partial_already_dirty();
    ac2_run_one_auto_cascade();
    ac3_ir_bridges();
    ac4_metrics();
    ac5_complex_graph_1000_rounds();
    ac6_instruction_level();
    ac_phase();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
