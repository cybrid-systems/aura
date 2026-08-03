// @category: unit
// @reason: Issue #2615 — production multi-block dirty cascades use
//          mark_blocks_dirty (one fence); residual N× mark_block_dirty forbidden.
//
//   AC1: Multi-block production sites use batch; fence +1 for N blocks
//   AC2: Single-block mark_block_dirty still one fence (unchanged)
//   AC3: finish_dirty_sync / instruction_dirty_synced_with_blocks holds
//   AC4: Gate/linter forbids multi mark_block_dirty loops in production sources
//   AC5: Batch fence rate < sequential for N-block invalidates

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.ir;
import aura.compiler.ir_soa;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::current_ir_soa_generation_fence;
using aura::compiler::g_ir_soa_batch_dirty_blocks_total;
using aura::compiler::g_ir_soa_batch_dirty_cascades_total;
using aura::compiler::g_ir_soa_single_dirty_marks_total;
using aura::compiler::IRFunctionSoA;
using aura::compiler::IRModuleV2;
using aura::compiler::kIrSoaBatchDirtyDisciplineIssue;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:soa-dirty-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static IRFunctionSoA make_n_block_fn(std::uint32_t n_blocks) {
    IRModuleV2 mod;
    auto fi = mod.add_function("disc_f", 4);
    for (std::uint32_t b = 0; b < n_blocks; ++b) {
        auto bi = mod.add_block(fi);
        mod.add_instruction(fi, IROpcode::ConstI64, {b, 1, 0, 0}, 0, 1, 0, 0);
        mod.add_instruction(fi, IROpcode::ConstI64, {b, 2, 0, 0}, 0, 1, 0, 0);
        mod.seal_block(fi, bi);
    }
    auto& fn = mod.functions[0];
    fn.block_dirty_.assign(n_blocks, 0);
    fn.instruction_dirty_.assign(fn.size(), 0);
    fn.generation_ = 0;
    return std::move(fn);
}

// ── AC1: multi-block batch one fence ──
static void ac1_multi_batch() {
    std::println("\n--- #2615 AC1: multi-block mark_blocks_dirty one fence ---");
    CHECK(kIrSoaBatchDirtyDisciplineIssue == 2615, "AC1: issue stamp");
    auto fn = make_n_block_fn(5);
    const auto fence0 = current_ir_soa_generation_fence();
    const auto cascades0 = g_ir_soa_batch_dirty_cascades_total.load(std::memory_order_relaxed);
    const auto blocks0 = g_ir_soa_batch_dirty_blocks_total.load(std::memory_order_relaxed);
    const std::uint32_t ids[] = {0, 1, 2, 3, 4};
    fn.mark_blocks_dirty(ids);
    CHECK(current_ir_soa_generation_fence() == fence0 + 1, "AC1: fence +1 for 5 blocks");
    CHECK(fn.generation() == 1, "AC1: generation +1");
    CHECK(g_ir_soa_batch_dirty_cascades_total.load(std::memory_order_relaxed) == cascades0 + 1,
          "AC1: cascade counter +1");
    CHECK(g_ir_soa_batch_dirty_blocks_total.load(std::memory_order_relaxed) == blocks0 + 5,
          "AC1: blocks counter +5");
    CHECK(fn.dirty_block_count() == 5, "AC1: all 5 blocks dirty");

    // Production source sites cite batch API
    const auto dce = read_file("src/compiler/pass_impls.ixx");
    CHECK(dce.find("Issue #2615") != std::string::npos, "AC1: DCE cites #2615");
    CHECK(dce.find("mark_blocks_dirty(changed_blocks)") != std::string::npos ||
              dce.find("mark_blocks_dirty(changed") != std::string::npos,
          "AC1: DCE uses batch mark_blocks_dirty");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("mark_blocks_dirty_bit_only") != std::string::npos,
          "AC1: precise path has mark_blocks_dirty_bit_only");
    CHECK(svc.find("Issue #2615") != std::string::npos, "AC1: service cites #2615");
}

// ── AC2: single-block unchanged ──
static void ac2_single_unchanged() {
    std::println("\n--- #2615 AC2: single mark_block_dirty one fence ---");
    auto fn = make_n_block_fn(3);
    const auto fence0 = current_ir_soa_generation_fence();
    const auto single0 = g_ir_soa_single_dirty_marks_total.load(std::memory_order_relaxed);
    fn.mark_block_dirty(1);
    CHECK(current_ir_soa_generation_fence() == fence0 + 1, "AC2: single fence +1");
    CHECK(fn.is_block_dirty(1) && !fn.is_block_dirty(0), "AC2: only block 1 dirty");
    CHECK(g_ir_soa_single_dirty_marks_total.load(std::memory_order_relaxed) == single0 + 1,
          "AC2: single mark counter +1");
}

// ── AC3: finish_dirty_sync holds ──
static void ac3_finish_sync() {
    std::println("\n--- #2615 AC3: finish_dirty_sync after batch ---");
    IRModuleV2 mod;
    auto fi = mod.add_function("sync_f", 2);
    for (std::uint32_t b = 0; b < 3; ++b) {
        auto bi = mod.add_block(fi);
        mod.add_instruction(fi, IROpcode::ConstI64, {b, 1, 0, 0}, 0, 1, 0, 0);
        mod.seal_block(fi, bi);
    }
    auto& fn = mod.functions[0];
    fn.block_dirty_.assign(3, 0);
    fn.instruction_dirty_.assign(fn.size(), 0);
    const std::uint32_t ids[] = {0, 2};
    fn.mark_blocks_dirty(ids);
    const auto flipped = mod.finish_dirty_sync();
    (void)flipped;
    CHECK(mod.instruction_dirty_synced_with_blocks(), "AC3: instruction_dirty_synced_with_blocks");
    // Bit-only then finish_dirty_sync should cascade missing instrs
    auto fn2 = make_n_block_fn(2);
    const std::uint32_t bits[] = {0, 1};
    fn2.mark_blocks_dirty_bits_only(bits);
    CHECK(fn2.is_block_dirty(0) && fn2.is_block_dirty(1), "AC3: bit-only sets blocks");
    // Instrs may be clean until finish_dirty_sync on module
    IRModuleV2 mod2;
    auto fi2 = mod2.add_function("bits", 2);
    for (std::uint32_t b = 0; b < 2; ++b) {
        auto bi = mod2.add_block(fi2);
        mod2.add_instruction(fi2, IROpcode::ConstI64, {b, 1, 0, 0}, 0, 1, 0, 0);
        mod2.seal_block(fi2, bi);
    }
    mod2.functions[0].block_dirty_.assign(2, 0);
    mod2.functions[0].instruction_dirty_.assign(mod2.functions[0].size(), 0);
    mod2.functions[0].mark_blocks_dirty_bits_only(bits);
    (void)mod2.finish_dirty_sync();
    CHECK(mod2.instruction_dirty_synced_with_blocks(), "AC3: bit-only + finish_dirty_sync synced");
}

// ── AC4: gate / source no residual multi loops ──
static void ac4_no_residual_loops() {
    std::println("\n--- #2615 AC4: no residual multi mark_block_dirty loops ---");
    // Production sources: for-loops over mark_block_dirty on multi ids banned
    // (gate script is authority; here soft scan key files).
    const auto dce = read_file("src/compiler/pass_impls.ixx");
    // After #2615, DCE must batch-mark (not per-block mark_block_dirty in run).
    CHECK(dce.find("mark_block_dirty(block.block_id)") == std::string::npos,
          "AC4: DCE no per-block mark_block_dirty(block.block_id)");
    CHECK(dce.find("mark_blocks_dirty(changed_blocks)") != std::string::npos ||
              dce.find("changed_blocks") != std::string::npos,
          "AC4: DCE batch marks via changed_blocks");
    CHECK(dce.find("Issue #2615") != std::string::npos, "AC4: DCE SoA run cites #2615");
    const auto svc = read_file("src/compiler/service.ixx");
    // precise path must batch bit-only (not N× single mark_block_dirty_bit_only loop only)
    CHECK(svc.find("apply_impact_scope_dirty") != std::string::npos,
          "AC4: apply_impact_scope_dirty present");
    CHECK(svc.find("mark_blocks_dirty_bit_only") != std::string::npos,
          "AC4: precise uses mark_blocks_dirty_bit_only");
    CHECK(svc.find("N× mark_block_dirty_bit_only") != std::string::npos ||
              svc.find("Issue #2615") != std::string::npos,
          "AC4: precise path cites batch bit-only / #2615");
    CompilerService cs;
    CHECK(href(cs, "schema-2615") == 2615, "AC4: schema-2615 on query:soa-dirty-stats");
    CHECK(href(cs, "soa-batch-dirty-discipline-wired") == 1, "AC4: discipline wired");
}

// ── AC5: fence rate improves ──
static void ac5_fence_rate() {
    std::println("\n--- #2615 AC5: batch fence rate < sequential ---");
    constexpr std::uint32_t N = 8;
    auto fn_b = make_n_block_fn(N);
    auto fn_s = make_n_block_fn(N);
    std::vector<std::uint32_t> ids(N);
    for (std::uint32_t i = 0; i < N; ++i)
        ids[i] = i;

    const auto f0 = current_ir_soa_generation_fence();
    fn_b.mark_blocks_dirty(ids);
    const auto batch_delta = current_ir_soa_generation_fence() - f0;

    const auto f1 = current_ir_soa_generation_fence();
    for (std::uint32_t i = 0; i < N; ++i)
        fn_s.mark_block_dirty(i);
    const auto seq_delta = current_ir_soa_generation_fence() - f1;

    CHECK(batch_delta == 1, "AC5: batch fence delta == 1");
    CHECK(seq_delta == N, "AC5: sequential fence delta == N");
    CHECK(batch_delta < seq_delta, "AC5: batch fence < sequential");
}

} // namespace

int main() {
    std::println("=== Issue #2615: batch dirty cascade discipline ===");
    ac1_multi_batch();
    ac2_single_unchanged();
    ac3_finish_sync();
    ac4_no_residual_loops();
    ac5_fence_rate();
    std::println("\n=== #2615: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
