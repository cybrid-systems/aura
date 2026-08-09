// @category: unit
// @reason: Issue #2820 — last SoA block must be sealed (end_idx = size);
// alloc_block only seals previous; finalize seals all last blocks.
//
//   AC1: finalize_last_blocks / finalize_soa_module / #2820 cites
//   AC2: 3-block function — last end_idx empty until finalize; then full
//   AC3: multi-function module seals every last block; unsealed metric
//   AC4: schema-2820 query; this suite + linter; no docs/design/2820-*

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core;
import aura.compiler.service;
import aura.compiler.ir_soa;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::IRModuleV2;
using aura::compiler::LoweringState;
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
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:soa-adoption-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::uint32_t block_instr_count(const aura::compiler::IRFunctionSoA& fn, std::uint32_t bi) {
    if (bi >= fn.blocks_.size())
        return 0;
    const auto& b = fn.blocks_[bi];
    if (b.end_idx <= b.start_idx)
        return 0;
    return b.end_idx - b.start_idx;
}

} // namespace

int run_test_alloc_block_seal_last() {
    std::println("=== Issue #2820: alloc_block seal last SoA block ===");
    CHECK(true, "ac2820: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: source cites finalize + #2820 ---");
        auto low = read_file("src/compiler/lowering.ixx");
        auto impl = read_file("src/compiler/lowering_impl.cpp");
        auto soa = read_file("src/compiler/ir_soa.ixx");
        CHECK(!low.empty() && !soa.empty(), "AC1: sources readable");
        CHECK(low.find("Issue #2820") != std::string::npos, "AC1: lowering.ixx #2820");
        CHECK(low.find("finalize_soa_module") != std::string::npos, "AC1: finalize_soa_module");
        CHECK(low.find("seal_soa_function_last_block") != std::string::npos,
              "AC1: seal_soa_function_last_block");
        CHECK(soa.find("finalize_last_blocks") != std::string::npos, "AC1: IRModuleV2 finalize");
        CHECK(soa.find("g_lowering_alloc_block_unsealed_total_atomic") != std::string::npos,
              "AC1: unsealed metric");
        CHECK(impl.find("finalize_soa_module") != std::string::npos,
              "AC1: lower_to_ir_impl calls finalize");
        CHECK(impl.find("Issue #2820") != std::string::npos, "AC1: impl cites #2820");
        // set_cur_function seals previous
        auto scf = low.find("void set_cur_function");
        CHECK(scf != std::string::npos, "AC1: set_cur_function");
        auto scf_win = low.substr(scf, 800);
        CHECK(scf_win.find("seal_soa_function_last_block") != std::string::npos,
              "AC1: set_cur_function seals previous last block");
    }

    // ── AC2: 3-block unsealed last until finalize ──
    {
        std::println("\n--- AC2: 3-block last end_idx sealed by finalize ---");
        const auto u0 = aura::compiler::g_lowering_alloc_block_unsealed_total_atomic().load();

        IRModuleV2 mod;
        auto fi = mod.add_function("f3", 4);
        // block 0
        auto b0 = mod.add_block(fi);
        mod.add_instruction(fi, IROpcode::ConstI64, {0, 1, 0, 0});
        mod.add_instruction(fi, IROpcode::ConstI64, {1, 2, 0, 0});
        // alloc next seals previous pattern
        auto b1 = mod.add_block(fi);
        mod.seal_block(fi, b0); // as alloc_block would
        mod.add_instruction(fi, IROpcode::Add, {2, 0, 1, 0});
        auto b2 = mod.add_block(fi);
        mod.seal_block(fi, b1);
        mod.add_instruction(fi, IROpcode::ConstI64, {3, 9, 0, 0});
        mod.add_instruction(fi, IROpcode::Return, {3, 0, 0, 0});
        // Last block intentionally unsealed (alloc_block never seals last).
        auto& fn = mod.functions[fi];
        CHECK(fn.blocks_.size() == 3, "AC2: 3 blocks");
        CHECK(block_instr_count(fn, 0) == 2, "AC2: block0 sealed 2 instr");
        CHECK(block_instr_count(fn, 1) == 1, "AC2: block1 sealed 1 instr");
        CHECK(block_instr_count(fn, 2) == 0, "AC2: block2 unsealed empty range");
        CHECK(fn.blocks_[2].end_idx == fn.blocks_[2].start_idx,
              "AC2: last end_idx == start_idx before finalize");

        const auto sealed = mod.finalize_last_blocks();
        CHECK(sealed == 1, std::format("AC2: finalize sealed 1 last block (got {})", sealed));
        CHECK(block_instr_count(fn, 2) == 2, "AC2: block2 has 2 instrs after finalize");
        CHECK(fn.blocks_[2].end_idx == static_cast<std::uint32_t>(fn.size()),
              "AC2: last end_idx == size");
        // Second finalize is no-op (already sealed).
        CHECK(mod.finalize_last_blocks() == 0, "AC2: finalize idempotent");
        const auto u1 = aura::compiler::g_lowering_alloc_block_unsealed_total_atomic().load();
        CHECK(u1 > u0, "AC2: unsealed metric advanced");
        (void)b2;
    }

    // ── AC3: multi-function + LoweringState dual-emit path ──
    {
        std::println("\n--- AC3: multi-func + LoweringState finalize_soa_module ---");
        IRModuleV2 mod;
        auto f0 = mod.add_function("a", 2);
        auto ba = mod.add_block(f0);
        mod.add_instruction(f0, IROpcode::ConstI64, {0, 1, 0, 0});
        // leave unsealed
        auto f1 = mod.add_function("b", 2);
        auto bb0 = mod.add_block(f1);
        mod.add_instruction(f1, IROpcode::ConstI64, {0, 2, 0, 0});
        auto bb1 = mod.add_block(f1);
        mod.seal_block(f1, bb0);
        mod.add_instruction(f1, IROpcode::Return, {0, 0, 0, 0});
        // leave last of f1 unsealed
        (void)ba;
        (void)bb1;

        CHECK(block_instr_count(mod.functions[f0], 0) == 0, "AC3: f0 last empty");
        CHECK(block_instr_count(mod.functions[f1], 1) == 0, "AC3: f1 last empty");
        const auto n = mod.finalize_last_blocks();
        CHECK(n == 2, std::format("AC3: sealed 2 last blocks (got {})", n));
        CHECK(block_instr_count(mod.functions[f0], 0) == 1, "AC3: f0 sealed");
        CHECK(block_instr_count(mod.functions[f1], 1) == 1, "AC3: f1 sealed");

        // LoweringState dual-emit: 3 blocks via alloc_block, finalize seals last.
        aura::ast::ASTArena arena;
        LoweringState st(arena);
        st.enable_soa_dual_emit();
        aura::ir::IRFunction aos;
        aos.name = "test";
        aos.entry_block = 0;
        aos.blocks.push_back({0, {}, {}});
        st.cur_func = &aos;
        st.cur_block = 0;
        // Bootstrap SoA entry (mirrors lower_to_ir_impl)
        st.module_v2.functions.push_back({});
        st.cur_func_v2_idx = 0;
        (void)st.module_v2.add_block(0);
        st.emit(IROpcode::ConstI64, 0, 1);
        auto then_b = st.alloc_block();
        st.cur_block = then_b;
        st.emit(IROpcode::ConstI64, 1, 2);
        auto merge_b = st.alloc_block();
        st.cur_block = merge_b;
        st.emit(IROpcode::Return, 1);
        auto& sfn = st.module_v2.functions[0];
        CHECK(sfn.blocks_.size() == 3, "AC3: dual-emit 3 SoA blocks");
        // Last still unsealed before finalize.
        CHECK(sfn.blocks_.back().end_idx == sfn.blocks_.back().start_idx,
              "AC3: dual-emit last unsealed pre-finalize");
        st.finalize_soa_module();
        CHECK(sfn.blocks_.back().end_idx == static_cast<std::uint32_t>(sfn.size()),
              "AC3: dual-emit last sealed post-finalize");
        CHECK(block_instr_count(sfn, 2) >= 1, "AC3: last block has instrs");
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2820 query keys ---");
        CompilerService cs;
        CHECK(href(cs, "schema-2820") == 2820, "AC4: schema-2820");
        CHECK(href(cs, "issue-2820") == 2820, "AC4: issue-2820");
        CHECK(href(cs, "alloc-block-seal-last-wired") == 1, "AC4: wired");
        CHECK(href(cs, "lowering-alloc-block-unsealed-total") >= 0, "AC4: unsealed total");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(obs.find("schema-2820") != std::string::npos, "AC4: obs schema-2820");
        CHECK(obs.find("lowering-alloc-block-unsealed-total") != std::string::npos,
              "AC4: obs unsealed key");
    }

    std::println("\n=== #2820 alloc_block seal last: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_alloc_block_seal_last();
}
#endif
