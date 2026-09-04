#pragma once

// Issue #3454: ProductionPureWrapPass SoA dirty-entry surface.
//
// concept_constraints.ixx cannot name BlockDirtyPred (defined later in
// pass_pipeline_core.ixx). It CAN import aura.compiler.ir_soa — cmake
// lists ir_soa.ixx before concept_constraints.ixx, and ir_soa does not
// import the concept module — so the concept type-checks the SAME
// aura::compiler::IRFunctionSoA / IRModuleV2 entities as InlinePass.
// A GMF incomplete-type forward would be a different type from the
// module export and would fail ProductionPureWrapPass on real Wraps.
//
// Preferred production signatures (defaulted pred/mask at the impl):
//   void run_on_dirty_blocks_only(IRModuleV2&, const DefineDirtyMaskView* = nullptr);
//   void run_on_dirty_blocks_only(IRFunctionSoA&, BlockDirtyPred = {});
// AoS run_on_dirty_blocks_only(aura::ir::IRFunction&) stays DirtySoAEntryPass
// (Soft/unit + EscapeAnalysisWrap grandfather). Issue #3488: CK/CF/TP/Shape
// provide run_on_dirty_blocks_only(IRModuleV2&) and satisfy
// ProductionPureWrapPass. BlockDirtyPred remains trivially copyable (#3042).

namespace aura::compiler::pass_soa_sig {

inline constexpr int kProductionPureWrapSoaIssue = 3454;
inline constexpr int kProductionPureWrapHotPackIssue = 3488;

} // namespace aura::compiler::pass_soa_sig
