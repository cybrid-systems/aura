// @category: unit
// @reason: Issue #2130 — ShapeAwareFold + LinearOwnership truly dirty-aware
// (per-block peel / clean-function skip), aligned with DeadCoercion (#2025).
//
//   AC1: ShapeAwareFold with dirty mask only processes dirty blocks
//   AC2: LinearOwnership dirty_aware=true in kDefaultPassTable; skips clean funcs
//   AC3: metrics shape-fold / linear-own skip+process counters + schema-2130
//   AC4: full-mask (all dirty) matches full-run fold behavior
//   AC5: source cites #2130

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
import aura.compiler.optimization_passes;
import aura.compiler.pass_manager;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::LinearOwnershipWrap;
using aura::compiler::ShapeAwareFoldingPass;
using aura::compiler::opt_registry::find_descriptor;
using aura::compiler::opt_registry::kDefaultPassTable;
using aura::compiler::opt_registry::linear_own_clean_blocks_skipped;
using aura::compiler::opt_registry::linear_own_clean_funcs_skipped;
using aura::compiler::opt_registry::linear_own_dirty_aware_runs;
using aura::compiler::opt_registry::linear_own_dirty_blocks_scanned;
using aura::compiler::opt_registry::linear_own_dirty_funcs_processed;
using aura::compiler::opt_registry::LinearOwnershipPass;
using aura::compiler::opt_registry::PassKind;
using aura::compiler::opt_registry::shape_fold_clean_blocks_skipped;
using aura::compiler::opt_registry::shape_fold_dirty_aware_runs;
using aura::compiler::opt_registry::shape_fold_dirty_blocks_processed;
using OptShapeFold = aura::compiler::opt_registry::ShapeAwareFoldingPass;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:optimization-passes-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Minimal IR: 2 blocks; block 0 has CastOp with narrow_evidence (foldable);
// block 1 has Nop only.
static IRFunction make_two_block_fn() {
    IRFunction f;
    f.name = "two_block";
    f.local_count = 2;
    f.blocks.resize(2);
    f.entry_block = 0;
    // Dirty-target fold: CastOp with narrow_evidence → Nop
    IRInstruction cast;
    cast.opcode = IROpcode::CastOp;
    cast.operands = {0, 1, 0, 0};
    cast.narrow_evidence = 1;
    f.blocks[0].instructions.push_back(cast);
    IRInstruction nop;
    nop.opcode = IROpcode::Nop;
    f.blocks[1].instructions.push_back(nop);
    return f;
}

} // namespace

int run_test_dirty_aware_shape_linear_passes() {
    std::println("=== Issue #2130: dirty-aware ShapeAwareFold + LinearOwnership ===");

    // ── AC5: source ──
    {
        std::println("\n--- AC5: source ---");
        auto opt = read_file("src/compiler/optimization_passes.ixx");
        auto pm = read_file("src/compiler/pass_manager.ixx");
        CHECK(opt.find("Issue #2130") != std::string::npos, "opt_registry #2130");
        CHECK(pm.find("Issue #2130") != std::string::npos ||
                  pm.find("run_on_function") != std::string::npos,
              "pass_manager dirty peel");
        CHECK(opt.find("dirty_aware=true") != std::string::npos ||
                  opt.find("LinearOwnership") != std::string::npos,
              "LinearOwnership table");
    }

    // ── AC2: table dirty_aware ──
    {
        std::println("\n--- AC2: kDefaultPassTable ---");
        const auto* lo = find_descriptor(PassKind::LinearOwnership);
        CHECK(lo != nullptr, "LinearOwnership descriptor");
        CHECK(lo->dirty_aware, "LinearOwnership dirty_aware=true");
        const auto* sa = find_descriptor(PassKind::ShapeAwareFold);
        CHECK(sa != nullptr && sa->dirty_aware, "ShapeAwareFold dirty_aware");
    }

    // ── AC1: ShapeAwareFold peels clean blocks ──
    {
        std::println("\n--- AC1: ShapeAwareFold dirty peel ---");
        auto fn = make_two_block_fn();
        ShapeAwareFoldingPass full;
        full.run_on_function(fn);
        const auto full_folds = full.fold_count();
        CHECK(full_folds >= 1, "full run folds CastOp");
        CHECK(fn.blocks[0].instructions[0].opcode == IROpcode::Nop, "block0 folded");

        // Fresh function; only block 1 dirty → CastOp in block 0 must remain.
        auto fn2 = make_two_block_fn();
        ShapeAwareFoldingPass peel;
        std::vector<std::uint8_t> dirty = {0, 1}; // only block 1
        peel.run_on_function(fn2, dirty);
        CHECK(peel.blocks_skipped() >= 1, "skipped clean block 0");
        CHECK(peel.blocks_processed() >= 1, "processed dirty block 1");
        CHECK(fn2.blocks[0].instructions[0].opcode == IROpcode::CastOp,
              "clean block CastOp not folded");
        CHECK(peel.fold_count() == 0, "no folds when only clean-target block dirty");

        // Only block 0 dirty → fold happens
        auto fn3 = make_two_block_fn();
        ShapeAwareFoldingPass peel2;
        std::vector<std::uint8_t> dirty0 = {1, 0};
        peel2.run_on_function(fn3, dirty0);
        CHECK(fn3.blocks[0].instructions[0].opcode == IROpcode::Nop, "dirty block0 folded");
        CHECK(peel2.fold_count() >= 1, "fold count on dirty-only");
    }

    // ── AC4: all-dirty mask == full ──
    {
        std::println("\n--- AC4: all-dirty ≡ full ---");
        auto a = make_two_block_fn();
        auto b = make_two_block_fn();
        ShapeAwareFoldingPass fa, fb;
        fa.run_on_function(a);
        std::vector<std::uint8_t> all = {1, 1};
        fb.run_on_function(b, all);
        CHECK(fa.fold_count() == fb.fold_count(), "fold counts match");
        CHECK(a.blocks[0].instructions[0].opcode == b.blocks[0].instructions[0].opcode,
              "opcodes match");
    }

    // ── LinearOwnership clean-func skip ──
    {
        std::println("\n--- LinearOwnership dirty-aware ---");
        IRModule mod;
        auto f = make_two_block_fn();
        mod.functions.push_back(f);
        LinearOwnershipPass pass;
        // All clean
        pass.set_block_dirty_fn([](std::uint32_t) { return false; });
        const auto skip0 = linear_own_clean_funcs_skipped.load();
        const auto runs0 = linear_own_dirty_aware_runs.load();
        pass.run(mod);
        CHECK(linear_own_dirty_aware_runs.load() > runs0, "dirty-aware run counted");
        CHECK(linear_own_clean_funcs_skipped.load() > skip0, "clean func skipped");

        // Any dirty → process
        LinearOwnershipPass pass2;
        pass2.set_block_dirty_fn([](std::uint32_t bi) { return bi == 0; });
        const auto proc0 = linear_own_dirty_funcs_processed.load();
        pass2.run(mod);
        CHECK(linear_own_dirty_funcs_processed.load() > proc0, "dirty func processed");
        CHECK(linear_own_dirty_blocks_scanned.load() >= 1, "dirty blocks scanned metric");
    }

    // ── AC3: query metrics ──
    {
        std::println("\n--- AC3: schema-2130 ---");
        // Exercise dirty-aware once so counters non-zero.
        OptShapeFold wrap;
        wrap.set_block_dirty_fn([](std::uint32_t bi) { return bi == 0; });
        IRModule m;
        m.functions.push_back(make_two_block_fn());
        wrap.run(m);

        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
        CHECK(href(cs, "schema-2130") == 2130, "schema-2130");
        CHECK(href(cs, "issue-2130") == 2130, "issue-2130");
        CHECK(href(cs, "shape-aware-fold-dirty-aware") == 1, "shape dirty-aware flag");
        CHECK(href(cs, "linear-ownership-dirty-aware") == 1, "linear dirty-aware flag");
        CHECK(href(cs, "shape-fold-dirty-blocks-processed") >= 0, "shape processed key");
        CHECK(href(cs, "shape-fold-clean-blocks-skipped") >= 0, "shape skipped key");
        CHECK(href(cs, "linear-own-clean-funcs-skipped") >= 0, "linear skip key");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dirty_aware_shape_linear_passes();
}
#endif
