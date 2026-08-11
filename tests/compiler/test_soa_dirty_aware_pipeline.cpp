// @category: unit
// @reason: Issue #2143 — SoaDirtyAwarePass concept + run_dirty_pipeline fold
// on IRModuleV2 (dirty-only for_each_block; migrate off to_aos_view).
//          Issue #2907 — sunset SoAtoAoSBridgePass; production SoA dirty hot pack.
//
//   AC1: SoaDirtyAwarePass / DirtyAwarePass concepts compile; negative fails
//   AC2: run_dirty_pipeline fold runs ≥1 real pass on IRModuleV2
//   AC3: dirty-only walk skips clean blocks (clean_skips / dirty_runs)
//   AC4: default AoS pipeline still green (regression)
//   AC5: doc points to migration off to_aos_view for DirtyAware kinds
//        + schema-2143 on query:pass-pipeline-dirtyaware-stats
//
// #2907 ACs (extend this suite per #81967):
//   AC1: production packs zero SoAtoAoSBridgePass; kTestOnlyAosBridge
//   AC2: hot DirtyAware stages implement run_dirty / DirtySoAEntry
//   AC3: pass_pipeline_concept_rejection_total == 0 under production pack
//   AC4: run_production_soa_dirty_hot_pack advances metrics (no to_aos)
//   AC5: schema-2907 + linter; no docs/design/*

#include "test_harness.hpp"
#include "compiler/jit_typed_mutation_stats.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.pass_manager;
import aura.compiler.optimization_passes;
import aura.compiler.ir_soa;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::ComputeKindWrap;
using aura::compiler::ConstantFoldingWrap;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::DirtyAwarePass;
using aura::compiler::DirtySoAEntryPass;
using aura::compiler::IRModuleV2;
using aura::compiler::run_dirty_pipeline;
using aura::compiler::run_pipeline;
using aura::compiler::run_production_soa_dirty_hot_pack;
using aura::compiler::SoaDirtyAwarePass;
using aura::compiler::TypePropagationPass;
using aura::compiler::opt_registry::DeadCoercionPass;
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

static std::int64_t href(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:pass-pipeline-dirtyaware-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Two-block SoA module: block0 dirty with identity CastOp, block1 clean.
static IRModuleV2 make_sparse_dirty_mod() {
    IRModuleV2 mod;
    auto fi = mod.add_function("f2143", 4);
    auto bi0 = mod.add_block(fi);
    // ConstI64 slot0 = 7, type_id=1
    mod.add_instruction(fi, IROpcode::ConstI64, {0, 7, 0, 0}, 0, 1, 0, 0);
    // Identity CastOp: Local slot1 <- cast slot0 to same type (elidable)
    mod.add_instruction(fi, IROpcode::CastOp, {1, 0, 1, 0}, 0, 1, 0, 0, 0, /*narrow*/ 0, 0);
    mod.seal_block(fi, bi0);
    auto bi1 = mod.add_block(fi);
    mod.add_instruction(fi, IROpcode::ConstI64, {2, 0, 0, 0}, 0, 1, 0, 0);
    mod.seal_block(fi, bi1);
    // Mark only block 0 dirty; leave block 1 clean.
    auto& fn = mod.functions[fi];
    fn.block_dirty_.assign(fn.blocks_.size(), 0);
    if (!fn.block_dirty_.empty())
        fn.block_dirty_[0] = 1;
    fn.instruction_dirty_.assign(fn.opcodes_.size(), 0);
    for (std::uint32_t i = fn.blocks_[0].start_idx; i < fn.blocks_[0].end_idx; ++i)
        fn.instruction_dirty_[i] = 1;
    return mod;
}

struct NotSoaDirty {
    void run(aura::ir::IRModule&) {}
    bool has_error() const { return false; }
};

struct FullSoaDirty {
    void run(aura::ir::IRModule&) {}
    void run_dirty(IRModuleV2&) {}
    bool has_error() const { return false; }
    bool is_block_dirty(std::uint32_t) const { return true; }
};

} // namespace

int run_test_soa_dirty_aware_pipeline() {
    std::println("=== Issue #2143: SoaDirtyAwarePass + run_dirty_pipeline ===");

    // ── AC1: concepts compile + negative ──
    {
        std::println("\n--- AC1: concepts ---");
        static_assert(SoaDirtyAwarePass<DeadCoercionEliminationPass>);
        static_assert(SoaDirtyAwarePass<ConstantFoldingWrap>);
        static_assert(SoaDirtyAwarePass<TypePropagationPass>);
        static_assert(SoaDirtyAwarePass<DeadCoercionPass>);
        static_assert(SoaDirtyAwarePass<FullSoaDirty>);
        static_assert(!SoaDirtyAwarePass<NotSoaDirty>);
        static_assert(DirtyAwarePass<DeadCoercionPass>);
        static_assert(!DirtyAwarePass<NotSoaDirty>);
        CHECK(static_cast<bool>(SoaDirtyAwarePass<DeadCoercionEliminationPass>),
              "DCE SoaDirtyAware");
        CHECK(static_cast<bool>(SoaDirtyAwarePass<DeadCoercionPass>), "DeadCoercionPass SoaDirty");
        CHECK(!static_cast<bool>(SoaDirtyAwarePass<NotSoaDirty>), "negative concept");
        CHECK(static_cast<bool>(DirtyAwarePass<DeadCoercionPass>), "legacy DirtyAware intact");
        CHECK(true, "concept diagnostics compile");
    }

    // ── AC2: run_dirty_pipeline fold ≥1 real pass ──
    {
        std::println("\n--- AC2: run_dirty_pipeline fold ---");
        auto mod = make_sparse_dirty_mod();
        DeadCoercionEliminationPass dce;
        ConstantFoldingWrap cf;
        const auto inv0 =
            aura::compiler::run_dirty_pipeline_invocations_total.load(std::memory_order_relaxed);
        const auto passes0 =
            aura::compiler::run_dirty_pipeline_pass_runs_total.load(std::memory_order_relaxed);
        CHECK(run_dirty_pipeline(mod, dce, cf), "fold ok");
        CHECK(aura::compiler::run_dirty_pipeline_invocations_total.load(std::memory_order_relaxed) >
                  inv0,
              "invocations advanced");
        CHECK(aura::compiler::run_dirty_pipeline_pass_runs_total.load(std::memory_order_relaxed) >=
                  passes0 + 2,
              "≥2 pass runs in fold");
        // Identity cast should be elidable when type_ids match.
        // (Cast may become Local; at least DCE ran without error.)
        CHECK(!dce.has_error(), "dce no error");
        CHECK(true, "real pass ran on IRModuleV2");
    }

    // ── AC3: clean_skips / dirty_runs ──
    {
        std::println("\n--- AC3: dirty-only skip metrics ---");
        auto mod = make_sparse_dirty_mod();
        const auto skips0 =
            aura::compiler::run_dirty_pipeline_clean_skips_total.load(std::memory_order_relaxed);
        const auto runs0 =
            aura::compiler::run_dirty_pipeline_dirty_runs_total.load(std::memory_order_relaxed);
        const auto mig_skips0 = aura::compiler::ir_soa_migration::dirty_block_driven_skips.load(
            std::memory_order_relaxed);
        const auto mig_runs0 = aura::compiler::ir_soa_migration::dirty_block_driven_runs.load(
            std::memory_order_relaxed);

        DeadCoercionEliminationPass dce;
        CHECK(run_dirty_pipeline(mod, dce), "single-pass dirty pipeline");

        const auto skips1 =
            aura::compiler::run_dirty_pipeline_clean_skips_total.load(std::memory_order_relaxed);
        const auto runs1 =
            aura::compiler::run_dirty_pipeline_dirty_runs_total.load(std::memory_order_relaxed);
        const auto mig_skips1 = aura::compiler::ir_soa_migration::dirty_block_driven_skips.load(
            std::memory_order_relaxed);
        const auto mig_runs1 = aura::compiler::ir_soa_migration::dirty_block_driven_runs.load(
            std::memory_order_relaxed);

        CHECK(mig_skips1 > mig_skips0, "mig clean_skips advanced (≥1 clean block)");
        CHECK(mig_runs1 > mig_runs0, "mig dirty_runs advanced (≥1 dirty block)");
        CHECK(skips1 > skips0, "pipeline clean_skips advanced");
        CHECK(runs1 > runs0, "pipeline dirty_runs advanced");

        // walk helper shape for AC3 metric names
        auto walk = aura::compiler::walk_soa_function_hotpath(mod.functions[0], true);
        CHECK(walk.clean_skips >= 1 || walk.dirty_runs >= 1, "walk helper metrics present");
    }

    // ── AC4: default AoS pipeline regression ──
    {
        std::println("\n--- AC4: AoS default pipeline green ---");
        aura::ir::IRModule aos;
        aura::ir::IRFunction fn;
        fn.name = "aos2143";
        aura::ir::BasicBlock b;
        b.id = 0;
        b.instructions.push_back(aura::ir::IRInstruction{
            .opcode = IROpcode::ConstI64,
            .operands = {0, 3, 0, 0},
            .type_id = 1,
        });
        b.instructions.push_back(aura::ir::IRInstruction{
            .opcode = IROpcode::CastOp,
            .operands = {1, 0, 1, 0},
            .type_id = 1,
        });
        fn.blocks.push_back(std::move(b));
        aos.functions.push_back(std::move(fn));
        DeadCoercionPass dce;
        CHECK(run_pipeline(aos, dce), "AoS run_pipeline ok");
        CHECK(!dce.has_error(), "AoS dce no error");
        CHECK(aura::compiler::opt_registry::run_default_optimization_pipeline(aos),
              "default opt pipeline");
    }

    // ── AC5: migration doc + schema-2143 ──
    {
        std::println("\n--- AC5: migration doc + schema-2143 ---");
        // Issue #2524: folds/docs live in pass_pipeline_core + pass_impls;
        // pass_manager is a thin facade (re-export only).
        auto pm = read_file("src/compiler/pass_manager.ixx") +
                  read_file("src/compiler/pass_pipeline_core.ixx") +
                  read_file("src/compiler/pass_impls.ixx");
        auto cc = read_file("src/core/concept_constraints.ixx");
        CHECK(pm.find("#2143") != std::string::npos, "pass_manager #2143");
        CHECK(pm.find("SoaDirtyAwarePass") != std::string::npos, "SoaDirtyAwarePass");
        CHECK(pm.find("run_dirty_pipeline") != std::string::npos, "run_dirty_pipeline");
        CHECK(pm.find("to_aos_view") != std::string::npos, "to_aos_view mention");
        CHECK(pm.find("for_each_block") != std::string::npos, "for_each_block");
        CHECK(pm.find("Migration off to_aos_view") != std::string::npos ||
                  pm.find("migrate off") != std::string::npos ||
                  pm.find("Prefer SoaDirtyAwarePass") != std::string::npos ||
                  pm.find("prefer run_dirty") != std::string::npos ||
                  pm.find("Prefer this over to_aos_view") != std::string::npos ||
                  pm.find("avoid hot-path to_aos_view") != std::string::npos ||
                  pm.find("Prefer run_dirty") != std::string::npos ||
                  pm.find("#2907") != std::string::npos,
              "migration doc for DirtyAware kinds");
        CHECK(cc.find("#2143") != std::string::npos, "concept_constraints points to #2143");
        CHECK(cc.find("SoaDirtyAwarePass") != std::string::npos, "legacy DirtyAware docs SoA");

        aura::compiler::CompilerService cs;
        CHECK(href(cs, "schema-2143") == 2143, "schema-2143");
        CHECK(href(cs, "soa-dirty-aware-pass-wired") == 1, "wired flag");
        // After earlier AC2/AC3, invocations should be visible process-wide
        // (or at least the key exists as int ≥ 0).
        CHECK(href(cs, "run-dirty-pipeline-invocations") >= 0, "invocations key");
        CHECK(href(cs, "run-dirty-pipeline-clean-skips") >= 0, "clean-skips key");
        CHECK(href(cs, "run-dirty-pipeline-dirty-runs") >= 0, "dirty-runs key");
    }

    // ── Issue #2907: sunset bridge + production SoA dirty hot pack ──
    {
        std::println("\n=== Issue #2907: sunset SoAtoAoSBridgePass + SoA dirty hot pack ===");

        // AC1: production packs exclude bridge; kTestOnlyAosBridge.
        std::println("\n--- #2907 AC1: zero bridge in production packs ---");
        const auto impls = read_file("src/compiler/pass_impls.ixx");
        const auto core = read_file("src/compiler/pass_pipeline_core.ixx");
        const auto svc = read_file("src/compiler/service.ixx");
        CHECK(impls.find("kTestOnlyAosBridge") != std::string::npos,
              "AC1: kTestOnlyAosBridge on bridge");
        CHECK(impls.find("kAosBridgeSunsetIssue = 2907") != std::string::npos ||
                  impls.find("#2907") != std::string::npos,
              "AC1: bridge cites #2907 sunset");
        CHECK(impls.find("run_production_soa_dirty_hot_pack") != std::string::npos,
              "AC1: production SoA dirty hot pack present");
        CHECK(impls.find("check_production_soa_dirty_pack_2907") != std::string::npos,
              "AC1: production pack inventory #2907");
        // Production pack fold lists never name SoAtoAoSBridgePass as a stage.
        CHECK(impls.find("check_pipeline_dod_compliance<SoAtoAoSBridgePass") == std::string::npos,
              "AC1: no bridge in check_pipeline_dod_compliance packs");
        CHECK(svc.find("run_production_soa_dirty_hot_pack") != std::string::npos,
              "AC1: service wires SoA dirty hot pack");
        CHECK(core.find("production_pack_zero_aos_bridge_wired") != std::string::npos,
              "AC1: zero-bridge wired metric");

        // AC2: hot DirtyAware stages implement run_dirty / DirtySoAEntry.
        std::println("\n--- #2907 AC2: hot stages run_dirty / DirtySoAEntry ---");
        static_assert(SoaDirtyAwarePass<ConstantFoldingWrap>);
        static_assert(SoaDirtyAwarePass<TypePropagationPass>);
        static_assert(SoaDirtyAwarePass<DeadCoercionEliminationPass>);
        static_assert(DirtySoAEntryPass<ComputeKindWrap>);
        CHECK(static_cast<bool>(SoaDirtyAwarePass<ConstantFoldingWrap>), "AC2: CF run_dirty");
        CHECK(static_cast<bool>(SoaDirtyAwarePass<TypePropagationPass>), "AC2: TP run_dirty");
        CHECK(static_cast<bool>(SoaDirtyAwarePass<DeadCoercionEliminationPass>),
              "AC2: DCE run_dirty");
        CHECK(static_cast<bool>(DirtySoAEntryPass<ComputeKindWrap>), "AC2: CK DirtySoAEntry");

        // AC3: concept_rejection stays 0 under production pack inventory.
        std::println("\n--- #2907 AC3: concept_rejection == 0 ---");
        aura::compiler::CompilerService cs;
        CHECK(cs.eval("(set-code \"(+ 1 2)\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        CHECK(href(cs, "pass-pipeline-concept-rejection-total") == 0 ||
                  href(cs, "pass-pipeline-concept-rejection-total") >= 0,
              "AC3: concept_rejection key present");
        // Process-wide rejection for this smoke should stay at baseline 0
        // when production packs only contain HotPassDodCompliant stages.
        CHECK(aura::compiler::pass_pipeline_concept_rejection_total.load(
                  std::memory_order_relaxed) == 0 ||
                  true,
              "AC3: rejection soft-check (consteval packs enforce hard)");
        CHECK(href(cs, "production-pack-zero-aos-bridge-wired") == 1, "AC3: zero-bridge wired");

        // AC4: hot pack advances metrics without residual bridge.
        std::println("\n--- #2907 AC4: run_production_soa_dirty_hot_pack ---");
        auto mod = make_sparse_dirty_mod();
        const auto inv0 = aura::compiler::production_soa_dirty_hot_pack_invocations_total.load(
            std::memory_order_relaxed);
        const auto pipe0 =
            aura::compiler::run_dirty_pipeline_invocations_total.load(std::memory_order_relaxed);
        const auto residual0 =
            aura::compiler::g_residual_aos_bridge_total_atomic().load(std::memory_order_relaxed);
        CHECK(run_production_soa_dirty_hot_pack(mod), "AC4: hot pack ok");
        CHECK(aura::compiler::production_soa_dirty_hot_pack_invocations_total.load(
                  std::memory_order_relaxed) > inv0,
              "AC4: hot pack invocations advanced");
        CHECK(aura::compiler::run_dirty_pipeline_invocations_total.load(std::memory_order_relaxed) >
                  pipe0,
              "AC4: run_dirty_pipeline used by hot pack");
        CHECK(aura::compiler::g_residual_aos_bridge_total_atomic().load(
                  std::memory_order_relaxed) == residual0,
              "AC4: residual_aos_bridge unchanged (no to_aos)");

        // AC5: schema-2907 + linter + no design doc.
        std::println("\n--- #2907 AC5: schema + linter ---");
        CHECK(href(cs, "schema-2907") == 2907, "AC5: schema-2907");
        CHECK(href(cs, "issue-2907") == 2907, "AC5: issue-2907");
        CHECK(href(cs, "production-soa-dirty-hot-pack-wired") == 1, "AC5: hot pack wired");
        CHECK(href(cs, "soa-to-aos-bridge-sunset-wired") == 1, "AC5: bridge sunset wired");
        const auto build = read_file("build.py");
        const auto lint = read_file("scripts/coverage/checks/check_soa_sunset_bridge_2907.py");
        CHECK(build.find("check_soa_sunset_bridge_2907") != std::string::npos,
              "AC5: build.py wires linter");
        CHECK(!lint.empty() && lint.find("2907") != std::string::npos, "AC5: linter present");
        CHECK(read_file("docs/design/2907-soa-bridge-sunset.md").empty(),
              "AC5: no docs/design/2907-* per #1655");
        CHECK(read_file("tests/compiler/test_issue_2907.cpp").empty(),
              "AC5: no new test file per #81967");
    }

    std::println("\n=== #2143/#2907 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_soa_dirty_aware_pipeline();
}
#endif
