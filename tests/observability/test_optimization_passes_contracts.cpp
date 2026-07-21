// @category: unit
// @reason: Issue #1576 — concrete optimization passes with C++26 contracts
// in optimization_passes.ixx (refine #1201).
//
//   AC1: 4 core passes satisfy Pass / DirtyAware / PureAnalysis where applicable
//   AC2: run() contracts pre-check module validity; metrics pre/post counts
//   AC3: PassDescriptor requires_contracts / pure flags
//   AC4: run_default_optimization_pipeline / run_pass_kind factory
//   AC5: query:optimization-passes-stats (schema 1576) via engine:metrics
//   AC6: sequential run of all 4 core passes on a synthetic IRModule

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.optimization_passes;
import aura.compiler.value;
import aura.compiler.service;

namespace {

using aura::compiler::opt_registry::ComputeKindPass;
using aura::compiler::opt_registry::ConstantFoldingPass;
using aura::compiler::opt_registry::default_pass_count;
using aura::compiler::opt_registry::find_descriptor;
using aura::compiler::opt_registry::kDefaultPassTable;
using aura::compiler::opt_registry::kOptimizationPassesPhase;
using aura::compiler::opt_registry::opt_contracts_post_checks_total;
using aura::compiler::opt_registry::opt_contracts_pre_checks_total;
using aura::compiler::opt_registry::opt_pass_runs_total;
using aura::compiler::opt_registry::opt_pipeline_factory_runs_total;
using aura::compiler::opt_registry::PassKind;
using aura::compiler::opt_registry::run_contracted_default_passes;
using aura::compiler::opt_registry::run_default_optimization_pipeline;
using aura::compiler::opt_registry::run_pass_kind;
using aura::compiler::opt_registry::ShapeAwareFoldingPass;
using aura::compiler::opt_registry::TypePropagationPass;
using aura::test::g_failed;
using aura::test::g_passed;

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static aura::ir::IRModule make_sample_module() {
    aura::ir::IRModule mod;
    aura::ir::IRFunction fn;
    fn.id = 0;
    fn.name = "f";
    fn.entry_block = 0;
    fn.arg_count = 1;
    fn.local_count = 2;
    aura::ir::BasicBlock bb;
    bb.id = 0;
    // ConstI64 1; ConstI64 2; Add; Return
    {
        aura::ir::IRInstruction c0{};
        c0.opcode = aura::ir::IROpcode::ConstI64;
        c0.operands[0] = 0;
        c0.operands[1] = 1;
        bb.instructions.push_back(c0);
        aura::ir::IRInstruction c1{};
        c1.opcode = aura::ir::IROpcode::ConstI64;
        c1.operands[0] = 1;
        c1.operands[1] = 2;
        bb.instructions.push_back(c1);
        aura::ir::IRInstruction add{};
        add.opcode = aura::ir::IROpcode::Add;
        add.operands[0] = 0;
        add.operands[1] = 0;
        add.operands[2] = 1;
        bb.instructions.push_back(add);
        aura::ir::IRInstruction ret{};
        ret.opcode = aura::ir::IROpcode::Return;
        ret.operands[0] = 0;
        bb.instructions.push_back(ret);
    }
    fn.blocks.push_back(std::move(bb));
    mod.functions.push_back(std::move(fn));
    mod.entry_function_id = 0;
    return mod;
}

static void ac1_concepts() {
    std::println("\n--- AC1: concepts on concrete passes ---");
    CHECK(static_cast<bool>(aura::compiler::Pass<ConstantFoldingPass>), "CF Pass");
    CHECK(static_cast<bool>(aura::compiler::DirtyAwarePass<ConstantFoldingPass>), "CF DirtyAware");
    CHECK(static_cast<bool>(aura::compiler::IncrementalPass<ConstantFoldingPass>),
          "CF Incremental");

    CHECK(static_cast<bool>(aura::compiler::Pass<TypePropagationPass>), "TP Pass");
    CHECK(static_cast<bool>(aura::compiler::DirtyAwarePass<TypePropagationPass>), "TP DirtyAware");
    CHECK(static_cast<bool>(aura::compiler::IncrementalPass<TypePropagationPass>),
          "TP Incremental");

    CHECK(static_cast<bool>(aura::compiler::Pass<ComputeKindPass>), "CK Pass");
    CHECK(static_cast<bool>(aura::compiler::DirtyAwarePass<ComputeKindPass>), "CK DirtyAware");
    CHECK(static_cast<bool>(aura::compiler::PureAnalysisPass<ComputeKindPass>), "CK PureAnalysis");

    CHECK(static_cast<bool>(aura::compiler::Pass<ShapeAwareFoldingPass>), "SA Pass");
    CHECK(static_cast<bool>(aura::compiler::DirtyAwarePass<ShapeAwareFoldingPass>),
          "SA DirtyAware");
}

static void ac2_contracts_and_metrics() {
    std::println("\n--- AC2: contracts pre/post metrics on run ---");
    auto mod = make_sample_module();
    const auto pre0 = load_u64(opt_contracts_pre_checks_total);
    const auto post0 = load_u64(opt_contracts_post_checks_total);
    const auto runs0 = load_u64(opt_pass_runs_total);

    ConstantFoldingPass cf;
    cf.run(mod);
    CHECK(!cf.has_error(), "CF no error");
    CHECK(load_u64(opt_pass_runs_total) > runs0, "pass-runs advanced");
    CHECK(load_u64(opt_contracts_pre_checks_total) > pre0, "pre-checks advanced");
    CHECK(load_u64(opt_contracts_post_checks_total) > post0, "post-checks advanced");

    // Structural invalid module: entry_function_id OOB → pre fails under
    // -fcontracts (observe/enforce). We still exercise the helper.
    aura::ir::IRModule bad;
    bad.entry_function_id = 99;
    CHECK(!aura::compiler::opt_registry::ir_module_structurally_valid(bad),
          "invalid module rejected by helper");
    CHECK(aura::compiler::opt_registry::valid_soa_view(mod), "sample module valid");
    CHECK(aura::compiler::opt_registry::pipeline_epoch_consistent(), "epoch consistent");
}

static void ac3_descriptor_flags() {
    std::println("\n--- AC3: PassDescriptor requires_contracts / pure ---");
    CHECK(kOptimizationPassesPhase >= 2, "phase >= 2");
    CHECK(default_pass_count() >= 4, "table has >= 4 entries");
    const auto* cf = find_descriptor(PassKind::ConstantFold);
    CHECK(cf && cf->requires_contracts && cf->dirty_aware, "CF contracts+dirty");
    const auto* ck = find_descriptor(PassKind::ComputeKind);
    CHECK(ck && ck->requires_contracts && ck->pure, "CK contracts+pure");
    const auto* tp = find_descriptor(PassKind::TypePropagation);
    CHECK(tp && tp->requires_contracts && tp->dirty_aware, "TP contracts+dirty");
    const auto* sa = find_descriptor(PassKind::ShapeAwareFold);
    CHECK(sa && sa->requires_contracts && sa->dirty_aware, "SA contracts+dirty");
    int contracted = 0;
    for (const auto& d : kDefaultPassTable)
        if (d.requires_contracts)
            ++contracted;
    CHECK(contracted >= 4, std::format(">=4 contracted descriptors (got {})", contracted));
}

static void ac4_factory_pipeline() {
    std::println("\n--- AC4: factory / default pipeline ---");
    auto mod = make_sample_module();
    const auto factory0 = load_u64(opt_pipeline_factory_runs_total);
    const auto runs0 = load_u64(opt_pass_runs_total);

    CHECK(run_default_optimization_pipeline(mod), "default pipeline ok");
    CHECK(load_u64(opt_pipeline_factory_runs_total) > factory0, "factory runs++");
    CHECK(load_u64(opt_pass_runs_total) >= runs0 + 4, ">=4 pass runs from default pipeline");

    auto mod2 = make_sample_module();
    CHECK(run_pass_kind(mod2, PassKind::ConstantFold), "run_pass_kind CF");
    CHECK(run_pass_kind(mod2, PassKind::TypePropagation), "run_pass_kind TP");
    CHECK(run_pass_kind(mod2, PassKind::ComputeKind), "run_pass_kind CK");
    CHECK(run_pass_kind(mod2, PassKind::ShapeAwareFold), "run_pass_kind SA");

    auto mod3 = make_sample_module();
    CHECK(run_contracted_default_passes(mod3), "run_contracted_default_passes");
}

static void ac5_stats_primitive() {
    std::println("\n--- AC5: query:optimization-passes-stats ---");
    aura::compiler::CompilerService cs;
    // Ensure at least one pass run so counters may be non-zero after AC4.
    auto r = cs.eval("(engine:metrics \"query:optimization-passes-stats\")");
    CHECK(r.has_value(), "metrics returns value");
    if (!r)
        return;
    CHECK(aura::compiler::types::is_hash(*r), "returns hash");
    auto schema =
        cs.eval("(hash-ref (engine:metrics \"query:optimization-passes-stats\") 'schema)");
    CHECK(schema.has_value() && aura::compiler::types::is_int(*schema) &&
              aura::compiler::types::as_int(*schema) == 1576,
          "schema == 1576");
    auto phase = cs.eval("(hash-ref (engine:metrics \"query:optimization-passes-stats\") 'phase)");
    CHECK(phase.has_value() && aura::compiler::types::is_int(*phase) &&
              aura::compiler::types::as_int(*phase) >= 2,
          "phase >= 2");
    auto runs =
        cs.eval("(hash-ref (engine:metrics \"query:optimization-passes-stats\") 'pass-runs)");
    CHECK(runs.has_value() && aura::compiler::types::is_int(*runs) &&
              aura::compiler::types::as_int(*runs) >= 0,
          "pass-runs field present");
}

static void ac6_sequential_core_passes() {
    std::println("\n--- AC6: sequential core passes on IRModule ---");
    auto mod = make_sample_module();
    ConstantFoldingPass cf;
    TypePropagationPass tp;
    ComputeKindPass ck;
    ShapeAwareFoldingPass sa;

    cf.run(mod);
    CHECK(!cf.has_error(), "CF");
    tp.run(mod);
    CHECK(!tp.has_error(), "TP");
    ck.run(mod);
    CHECK(!ck.has_error(), "CK");
    sa.run(mod);
    CHECK(!sa.has_error(), "SA");

    // DirtyAware install works
    cf.set_block_dirty_fn([](std::uint32_t) { return true; });
    CHECK(cf.is_block_dirty(0), "CF dirty fn");
    CHECK(aura::compiler::run_pipeline(mod, cf, tp), "run_pipeline CF+TP");
}

} // namespace

int main() {
    std::println("=== test_optimization_passes_contracts (#1576) ===");
    ac1_concepts();
    ac2_contracts_and_metrics();
    ac3_descriptor_flags();
    ac4_factory_pipeline();
    ac5_stats_primitive();
    ac6_sequential_core_passes();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
