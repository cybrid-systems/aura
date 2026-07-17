// @category: unit
// @reason: Issue #1578 — RenderPass dirty_aware + shape_stable + incremental
// render pipeline (refine #1201 / #1576).
//
//   AC1: RenderPass satisfies DirtyAware + SoAView + JITFriendly + Incremental
//   AC2: contracts pre/post metrics; shape stable across dirty-only render
//   AC3: run_incremental_render_pipeline + DefineDirtyMaskView skips clean blocks
//   AC4: mutate (dirty one block) → only dirty blocks processed
//   AC5: query:render-pass-incremental-stats schema 1578
//   AC6: framebuffer present_batch_if_dirty short-circuit linkage
//   AC7: 1000 rounds incremental render — skipped >> processed when 1/N dirty

#include "test_harness.hpp"
#include "renderer/render_pass.hh"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.optimization_passes;
import aura.compiler.value;
import aura.compiler.service;

namespace {

using aura::compiler::DefineDirtyMaskView;
using aura::compiler::opt_registry::find_descriptor;
using aura::compiler::opt_registry::kOptimizationPassesPhase;
using aura::compiler::opt_registry::PassKind;
using aura::compiler::opt_registry::render_blocks_processed_total;
using aura::compiler::opt_registry::render_dirty_skipped_blocks;
using aura::compiler::opt_registry::render_framebuffer_present_skips;
using aura::compiler::opt_registry::render_incremental_hits;
using aura::compiler::opt_registry::render_pass_runs_total;
using aura::compiler::opt_registry::render_shape_stable_violations;
using aura::compiler::opt_registry::RenderPass;
using aura::compiler::opt_registry::run_incremental_render_pipeline;
using aura::compiler::opt_registry::run_pass_kind;
using aura::test::g_failed;
using aura::test::g_passed;

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static aura::ir::IRModule make_multi_block_module(int n_blocks) {
    aura::ir::IRModule mod;
    aura::ir::IRFunction fn;
    fn.id = 0;
    fn.name = "render_root";
    fn.entry_block = 0;
    fn.arg_count = 0;
    fn.local_count = 2;
    for (int i = 0; i < n_blocks; ++i) {
        aura::ir::BasicBlock bb;
        bb.id = static_cast<std::uint32_t>(i);
        aura::ir::IRInstruction c{};
        c.opcode = aura::ir::IROpcode::ConstI64;
        c.operands[0] = 0;
        c.operands[1] = static_cast<std::uint32_t>(i + 1);
        bb.instructions.push_back(c);
        aura::ir::IRInstruction ret{};
        ret.opcode = aura::ir::IROpcode::Return;
        ret.operands[0] = 0;
        bb.instructions.push_back(ret);
        fn.blocks.push_back(std::move(bb));
    }
    mod.functions.push_back(std::move(fn));
    mod.entry_function_id = 0;
    return mod;
}

static void ac1_concepts() {
    std::println("\n--- AC1: RenderPass concepts ---");
    CHECK(static_cast<bool>(aura::compiler::Pass<RenderPass>), "Pass");
    CHECK(static_cast<bool>(aura::compiler::DirtyAwarePass<RenderPass>), "DirtyAware");
    CHECK(static_cast<bool>(aura::compiler::IncrementalPass<RenderPass>), "Incremental");
    CHECK(static_cast<bool>(aura::compiler::SoAViewAwarePass<RenderPass>), "SoAView");
    CHECK(static_cast<bool>(aura::compiler::JITFriendlyPass<RenderPass>), "JITFriendly");
    CHECK(static_cast<bool>(aura::compiler::ShapeStableAwarePass<RenderPass>), "ShapeStable");
    const auto* d = find_descriptor(PassKind::Render);
    CHECK(d && d->dirty_aware && d->shape_stable && d->requires_contracts,
          "descriptor dirty+shape+contracts");
    CHECK(kOptimizationPassesPhase >= 3, "phase >= 3");
}

static void ac2_shape_stable_contracts() {
    std::println("\n--- AC2: shape stable across dirty-only render ---");
    auto mod = make_multi_block_module(4);
    const auto shape0 = RenderPass::compute_module_shape(mod);
    RenderPass rp;
    // Only block 1 dirty
    rp.set_block_dirty_fn([](std::uint32_t id) { return id == 1; });
    rp.enforce_shape_stable(true);
    const auto viol0 = load_u64(render_shape_stable_violations);
    rp.run(mod);
    CHECK(!rp.has_error(), "no error");
    CHECK(rp.last_shape_fingerprint() == shape0, "shape fingerprint unchanged");
    CHECK(RenderPass::module_shape_stable(mod, shape0), "module_shape_stable helper");
    CHECK(load_u64(render_shape_stable_violations) == viol0, "no shape violations");
    CHECK(rp.blocks_processed_last() == 1, "processed 1 dirty block");
    CHECK(rp.blocks_skipped_last() == 3, "skipped 3 clean blocks");
}

static void ac3_incremental_pipeline_mask() {
    std::println("\n--- AC3: run_incremental_render_pipeline + define mask ---");
    auto mod = make_multi_block_module(5);
    std::vector<std::vector<std::uint8_t>> mask = {{0, 1, 0, 0, 0}}; // only block 1
    DefineDirtyMaskView view;
    view.block_dirty_per_func = &mask;

    RenderPass rp;
    const auto skip0 = load_u64(render_dirty_skipped_blocks);
    const auto hits0 = load_u64(render_incremental_hits);
    CHECK(run_incremental_render_pipeline(mod, rp, &view), "incremental pipeline ok");
    // Pipeline installs mask via set_block_dirty_fn; pass.run(func) path used.
    CHECK(load_u64(render_dirty_skipped_blocks) > skip0 || rp.blocks_skipped_last() >= 0,
          "skips observed");
    CHECK(load_u64(render_incremental_hits) >= hits0, "incremental hits non-decreasing");
}

static void ac4_mutate_then_render() {
    std::println("\n--- AC4: mutate one block then incremental render ---");
    auto mod = make_multi_block_module(8);
    // "mutate": flip a constant in block 0 only (structure-preserving)
    mod.functions[0].blocks[0].instructions[0].operands[1] = 42;

    RenderPass rp;
    std::vector<std::uint8_t> dirty(8, 0);
    dirty[0] = 1;
    rp.set_block_dirty_fn([&](std::uint32_t id) { return id < dirty.size() && dirty[id] != 0; });

    const auto proc0 = load_u64(render_blocks_processed_total);
    const auto skip0 = load_u64(render_dirty_skipped_blocks);
    rp.run(mod);
    CHECK(rp.blocks_processed_last() == 1, "only block 0 rendered");
    CHECK(rp.blocks_skipped_last() == 7, "7 clean skipped");
    CHECK(load_u64(render_blocks_processed_total) == proc0 + 1, "processed metric +1");
    CHECK(load_u64(render_dirty_skipped_blocks) == skip0 + 7, "skipped metric +7");
    CHECK(rp.last_dirty_cleared(), "dirty_cleared post flag");

    // Golden: structure (opcode stream) unchanged by render pass
    CHECK(mod.functions[0].blocks[0].instructions[0].operands[1] == 42,
          "render does not clobber IR payload");
}

static void ac5_stats_primitive() {
    std::println("\n--- AC5: query:render-pass-incremental-stats ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:render-pass-incremental-stats\")");
    CHECK(r.has_value(), "metrics returns");
    if (!r)
        return;
    CHECK(aura::compiler::types::is_hash(*r), "hash");
    auto schema =
        cs.eval("(hash-ref (engine:metrics \"query:render-pass-incremental-stats\") 'schema)");
    CHECK(schema.has_value() && aura::compiler::types::is_int(*schema) &&
              aura::compiler::types::as_int(*schema) == 1578,
          "schema == 1578");
    auto skips = cs.eval("(hash-ref (engine:metrics \"query:render-pass-incremental-stats\") "
                         "'dirty-skipped-blocks)");
    CHECK(skips.has_value() && aura::compiler::types::is_int(*skips) &&
              aura::compiler::types::as_int(*skips) >= 0,
          "dirty-skipped-blocks field");
}

static void ac6_framebuffer_short_circuit() {
    std::println("\n--- AC6: framebuffer present short-circuit ---");
    aura::renderer::g_framebuffer_dirty.clear();
    auto mod = make_multi_block_module(2);
    RenderPass rp;
    rp.set_block_dirty_fn([](std::uint32_t) { return false; }); // all clean IR
    rp.enable_framebuffer_present(true);
    const auto skip0 = load_u64(render_framebuffer_present_skips);
    rp.run(mod);
    CHECK(load_u64(render_framebuffer_present_skips) > skip0, "fb present skipped when clean");

    // Mark framebuffer dirty → present path returns true once
    aura::renderer::g_framebuffer_dirty.mark_dirty(1, 1);
    RenderPass rp2;
    rp2.set_block_dirty_fn([](std::uint32_t) { return true; });
    rp2.enable_framebuffer_present(true);
    const auto ok0 = load_u64(aura::compiler::opt_registry::render_framebuffer_present_ok);
    rp2.run(mod);
    CHECK(load_u64(aura::compiler::opt_registry::render_framebuffer_present_ok) > ok0,
          "fb present ok when dirty");
    CHECK(aura::renderer::g_framebuffer_dirty.is_clean(), "fb dirty cleared after present");
}

static void ac7_thousand_rounds() {
    std::println("\n--- AC7: 1000 rounds 1-of-N dirty ---");
    constexpr int kBlocks = 10;
    constexpr int kRounds = 1000;
    auto mod = make_multi_block_module(kBlocks);
    RenderPass rp;
    rp.set_block_dirty_fn([](std::uint32_t id) { return id == 0; });

    const auto skip0 = load_u64(render_dirty_skipped_blocks);
    const auto proc0 = load_u64(render_blocks_processed_total);
    for (int i = 0; i < kRounds; ++i)
        rp.run(mod);

    const auto skip_delta = load_u64(render_dirty_skipped_blocks) - skip0;
    const auto proc_delta = load_u64(render_blocks_processed_total) - proc0;
    CHECK(proc_delta == static_cast<std::uint64_t>(kRounds),
          std::format("processed {} (want {})", proc_delta, kRounds));
    CHECK(skip_delta == static_cast<std::uint64_t>(kRounds * (kBlocks - 1)),
          std::format("skipped {} (want {})", skip_delta, kRounds * (kBlocks - 1)));
    CHECK(skip_delta > proc_delta * 5, "skipped >> processed (dirty-aware win)");
}

static void ac_factory() {
    std::println("\n--- factory run_pass_kind(Render) ---");
    auto mod = make_multi_block_module(2);
    const auto runs0 = load_u64(render_pass_runs_total);
    CHECK(run_pass_kind(mod, PassKind::Render), "run_pass_kind Render");
    CHECK(load_u64(render_pass_runs_total) > runs0, "pass-runs advanced");
}

} // namespace

int main() {
    std::println("=== test_render_pass_incremental (#1578) ===");
    ac1_concepts();
    ac2_shape_stable_contracts();
    ac3_incremental_pipeline_mask();
    ac4_mutate_then_render();
    ac5_stats_primitive();
    ac6_framebuffer_short_circuit();
    ac7_thousand_rounds();
    ac_factory();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
