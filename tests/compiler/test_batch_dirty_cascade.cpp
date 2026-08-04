// @category: unit
// @reason: Issue #2522 — batch dirty cascade API (mark_blocks_dirty +
// single generation / g_ir_soa_generation_fence bump).
//
//   AC1: Batch API exists; one generation bump per call regardless of N
//   AC2: Semantics match sequential mark_block_dirty (blocks + instrs)
//   AC3: mark_all_blocks_dirty bulk path single bump (no double-bump)
//   AC4: N-block batch advances fence less than N× single marks
//   AC5: finish_dirty_sync still holds instruction_dirty_synced_with_blocks

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.ir_soa;
import aura.compiler.ir;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::current_ir_soa_generation_fence;
using aura::compiler::IRFunctionSoA;
using aura::compiler::IRModuleV2;
using aura::compiler::kIrSoaBatchDirtyIssue;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
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

// Build a function with N blocks, 2 instructions each.
static IRFunctionSoA make_n_block_fn(std::uint32_t n_blocks) {
    IRModuleV2 mod;
    auto fi = mod.add_function("batch_f", 4);
    for (std::uint32_t b = 0; b < n_blocks; ++b) {
        auto bi = mod.add_block(fi);
        mod.add_instruction(fi, IROpcode::ConstI64, {b, 1, 0, 0}, 0, 1, 0, 0);
        mod.add_instruction(fi, IROpcode::ConstI64, {b, 2, 0, 0}, 0, 1, 0, 0);
        mod.seal_block(fi, bi);
    }
    auto& fn = mod.functions[0];
    // Start clean.
    fn.block_dirty_.assign(n_blocks, 0);
    fn.instruction_dirty_.assign(fn.size(), 0);
    fn.generation_ = 0;
    return std::move(fn);
}

// ── AC1: batch API + one bump ──
static void ac1_batch_one_bump() {
    std::println("\n--- AC1: mark_blocks_dirty one generation bump ---");
    CHECK(kIrSoaBatchDirtyIssue == 2522, "AC1: issue stamp");
    const auto hh = read_file("src/compiler/ir_soa.ixx");
    CHECK(hh.find("Issue #2522") != std::string::npos, "AC1: #2522 cited");
    CHECK(hh.find("mark_blocks_dirty") != std::string::npos, "AC1: mark_blocks_dirty API");
    CHECK(hh.find("mark_instruction_range_dirty") != std::string::npos,
          "AC1: mark_instruction_range_dirty API");
    CHECK(hh.find("kIrSoaBatchDirtyIssue") != std::string::npos, "AC1: stamp constant");

    auto fn = make_n_block_fn(4);
    const auto g0 = fn.generation();
    const auto fence0 = current_ir_soa_generation_fence();
    const std::uint32_t ids[] = {0, 1, 2, 3};
    fn.mark_blocks_dirty(ids);
    CHECK(fn.generation() == g0 + 1, "AC1: generation +1 for 4 blocks");
    CHECK(current_ir_soa_generation_fence() == fence0 + 1, "AC1: fence +1 for 4 blocks");
    // Empty span: no bump
    const auto g1 = fn.generation();
    const auto fence1 = current_ir_soa_generation_fence();
    fn.mark_blocks_dirty(std::span<const std::uint32_t>{});
    CHECK(fn.generation() == g1, "AC1: empty span no generation bump");
    CHECK(current_ir_soa_generation_fence() == fence1, "AC1: empty span no fence bump");
}

// ── AC2: semantics match sequential ──
static void ac2_semantics_match() {
    std::println("\n--- AC2: batch semantics == sequential mark_block_dirty ---");
    auto batch = make_n_block_fn(3);
    auto seq = make_n_block_fn(3);
    const std::uint32_t ids[] = {0, 2}; // skip middle
    batch.mark_blocks_dirty(ids);
    seq.mark_block_dirty(0);
    seq.mark_block_dirty(2);

    for (std::uint32_t bi = 0; bi < 3; ++bi) {
        CHECK(batch.is_block_dirty(bi) == seq.is_block_dirty(bi),
              std::format("AC2: block {} dirty parity", bi));
    }
    CHECK(batch.is_block_dirty(0) && batch.is_block_dirty(2) && !batch.is_block_dirty(1),
          "AC2: only listed blocks dirty");
    CHECK(batch.dirty_instruction_count() == seq.dirty_instruction_count(),
          "AC2: instr dirty count match");
    // Each listed block's instr range dirty
    for (std::uint32_t bi : ids) {
        const auto& b = batch.blocks_[bi];
        for (std::uint32_t i = b.start_idx; i < b.end_idx; ++i)
            CHECK(batch.is_instruction_dirty(i),
                  std::format("AC2: instr {} in block {} dirty", i, bi));
    }
    // Middle block instrs clean
    const auto& mid = batch.blocks_[1];
    for (std::uint32_t i = mid.start_idx; i < mid.end_idx; ++i)
        CHECK(!batch.is_instruction_dirty(i), std::format("AC2: mid instr {} clean", i));
}

// ── AC3: full-function bulk single bump ──
static void ac3_full_function_single_bump() {
    std::println("\n--- AC3: mark_all_blocks_dirty single bump ---");
    auto fn = make_n_block_fn(5);
    const auto g0 = fn.generation();
    const auto fence0 = current_ir_soa_generation_fence();
    fn.mark_all_blocks_dirty();
    CHECK(fn.generation() == g0 + 1, "AC3: generation +1");
    CHECK(current_ir_soa_generation_fence() == fence0 + 1, "AC3: fence +1");
    CHECK(fn.dirty_block_count() == 5, "AC3: all blocks dirty");
    CHECK(fn.dirty_instruction_count() == fn.size(), "AC3: all instrs dirty");
    const auto hh = read_file("src/compiler/ir_soa.ixx");
    CHECK(hh.find("std::fill(block_dirty_") != std::string::npos ||
              hh.find("std::fill(block_dirty_.begin()") != std::string::npos,
          "AC3: bulk fill block_dirty");
    CHECK(hh.find("no double-bump") != std::string::npos ||
              hh.find("single bump") != std::string::npos,
          "AC3: single-bump documented");
}

// ── AC4: fence advances fewer for batch vs N× single ──
static void ac4_fence_fewer_advances() {
    std::println("\n--- AC4: batch fence advances < N× single ---");
    constexpr std::uint32_t N = 8;
    auto fn_batch = make_n_block_fn(N);
    auto fn_seq = make_n_block_fn(N);

    const auto fence_b0 = current_ir_soa_generation_fence();
    std::vector<std::uint32_t> all(N);
    for (std::uint32_t i = 0; i < N; ++i)
        all[i] = i;
    fn_batch.mark_blocks_dirty(all);
    const auto fence_batch_delta = current_ir_soa_generation_fence() - fence_b0;
    CHECK(fence_batch_delta == 1, "AC4: batch fence delta == 1");
    CHECK(fn_batch.generation() == 1, "AC4: batch generation == 1");

    const auto fence_s0 = current_ir_soa_generation_fence();
    for (std::uint32_t i = 0; i < N; ++i)
        fn_seq.mark_block_dirty(i);
    const auto fence_seq_delta = current_ir_soa_generation_fence() - fence_s0;
    CHECK(fence_seq_delta == N, "AC4: sequential fence delta == N");
    CHECK(fn_seq.generation() == N, "AC4: sequential generation == N");
    CHECK(fence_batch_delta < fence_seq_delta, "AC4: batch fence < sequential fence");
    std::println("  batch fence Δ={}  sequential fence Δ={}", fence_batch_delta, fence_seq_delta);

    // mark_instruction_range_dirty also one bump
    auto fn_r = make_n_block_fn(2);
    const auto gr0 = fn_r.generation();
    fn_r.mark_instruction_range_dirty(0, 3);
    CHECK(fn_r.generation() == gr0 + 1, "AC4: range dirty one bump");
    CHECK(fn_r.is_instruction_dirty(0) && fn_r.is_instruction_dirty(1) &&
              fn_r.is_instruction_dirty(2),
          "AC4: range filled");
}

// ── AC5: finish_dirty_sync holds synced invariant ──
static void ac5_finish_dirty_sync() {
    std::println("\n--- AC5: finish_dirty_sync after batch ---");
    IRModuleV2 mod;
    auto fi = mod.add_function("sync_f", 2);
    auto b0 = mod.add_block(fi);
    mod.add_instruction(fi, IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1, 0, 0);
    mod.add_instruction(fi, IROpcode::ConstI64, {1, 2, 0, 0}, 0, 1, 0, 0);
    mod.seal_block(fi, b0);
    auto b1 = mod.add_block(fi);
    mod.add_instruction(fi, IROpcode::Add, {2, 0, 1, 0}, 0, 1, 0, 0);
    mod.seal_block(fi, b1);
    auto& fn = mod.functions[0];
    fn.block_dirty_.assign(2, 0);
    fn.instruction_dirty_.assign(fn.size(), 0);

    const std::uint32_t ids[] = {0, 1};
    fn.mark_blocks_dirty(ids);
    CHECK(mod.instruction_dirty_synced_with_blocks(), "AC5: synced after batch");
    const auto flipped = mod.finish_dirty_sync();
    CHECK(flipped == 0, "AC5: finish_dirty_sync no residual flips");
    CHECK(mod.instruction_dirty_synced_with_blocks(), "AC5: still synced");
    CHECK(mod.count_block_instr_dirty_desync() == 0, "AC5: desync == 0");

    // Module-level batch helper
    auto fn2 = make_n_block_fn(2);
    IRModuleV2 mod2;
    mod2.functions.push_back(std::move(fn2));
    const std::uint32_t ids2[] = {0, 1};
    mod2.mark_function_blocks_dirty(0, ids2);
    CHECK(mod2.functions[0].is_block_dirty(0) && mod2.functions[0].is_block_dirty(1),
          "AC5: mark_function_blocks_dirty");
    (void)mod2.finish_dirty_sync();
    CHECK(mod2.instruction_dirty_synced_with_blocks(), "AC5: module synced");

    // Query surface
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:soa-dirty-stats\")");
    CHECK(h && is_hash(*h), "AC5: soa-dirty-stats hash");
    CHECK(href(cs, "schema-2522") == 2522, "AC5: schema-2522");
    CHECK(href(cs, "soa-batch-dirty-wired") == 1, "AC5: batch-dirty-wired");
    CHECK(href(cs, "schema-2139") == 2139, "AC5: retain 2139 lineage");

    // Production routing cites
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("mark_blocks_dirty") != std::string::npos, "AC5: service batch API");
    CHECK(svc.find("Issue #2522") != std::string::npos || svc.find("#2522") != std::string::npos,
          "AC5: service cites #2522");
    CHECK(svc.find("force_soa_instruction_dirty_sync") != std::string::npos,
          "AC5: force_soa retained");
}

} // namespace

int run_test_batch_dirty_cascade() {
    std::println("=== Issue #2522: batch dirty cascade (mark_blocks_dirty) ===");
    ac1_batch_one_bump();
    ac2_semantics_match();
    ac3_full_function_single_bump();
    ac4_fence_fewer_advances();
    ac5_finish_dirty_sync();
    std::println("\n=== #2522: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_batch_dirty_cascade();
}
#endif
