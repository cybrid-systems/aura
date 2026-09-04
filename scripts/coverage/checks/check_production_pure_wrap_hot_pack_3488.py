#!/usr/bin/env python3
"""Issue #3488: production DirtyAware PureWrap pack peels SoA dirty blocks.

#3454 left CK/CF/TP/Shape as DirtySoAEntryPass(IRFunction&) grandfather.
ProductionPureWrapPass was compile-true only for stubs. Those four wraps
now provide run_on_dirty_blocks_only(IRModuleV2&) and satisfy
ProductionPureWrapPass. AoS DirtySoAEntryPass stays Soft/unit.
EscapeAnalysisWrap remains the #3454 grandfather. No new query key.

Contract:
  AC1 CK/CF/TP/Shape static_assert ProductionPureWrapPass
  AC2 run_production_soa_pure_wrap_pack constrained; AoS-only fails
  AC3 Soft keeps DirtySoAEntryPass + AoS incremental suite
  AC4 SoA dirty peel records skip/run; no run(IRModule&) on clean
  AC5 extend soa dirty pipeline / #3454 / #3329; no invent

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    concepts = _read("src/core/concept_constraints.ixx")
    sig = _read("src/compiler/pass_soa_sig.hh")
    core = _read("src/compiler/pass_pipeline_core.ixx")
    impls = _read("src/compiler/pass_impls.ixx")
    svc = _read("src/compiler/service.ixx")
    t = _read("tests/compiler/test_soa_dirty_aware_pipeline.cpp")
    opt = _read("tests/compiler/test_optimization_passes_contracts.cpp")
    l3454 = _read("scripts/coverage/checks/check_production_pure_wrap_soa_3454.py")
    l3329 = _read("scripts/coverage/checks/check_production_pipeline_purity_3329.py")
    build = _read("build.py")

    must("kProductionPureWrapHotPackIssue = 3488", "AC1 stamp", concepts)
    must("kProductionPureWrapHotPackIssue = 3488", "AC1 header stamp", sig)
    must("static_assert(ProductionPureWrapPass<ComputeKindWrap>", "AC1 CK", impls)
    must("static_assert(ProductionPureWrapPass<ConstantFoldingWrap>", "AC1 CF", impls)
    must("static_assert(ProductionPureWrapPass<TypePropagationPass>", "AC1 TP", impls)
    must("static_assert(ProductionPureWrapPass<ShapeWrap>", "AC1 Shape", impls)

    ck = impls.find("export class ComputeKindWrap")
    ck_end = impls.find("export class ArityWrap", ck) if ck >= 0 else -1
    ck_body = impls[ck:ck_end] if ck >= 0 and ck_end > ck else ""
    must("void run_on_dirty_blocks_only(IRModuleV2& mod)", "AC1 CK SoA entry", ck_body)
    must(
        "void run_on_dirty_blocks_only(aura::ir::IRFunction& func, BlockDirtyPred pred = {})",
        "AC3 CK AoS kept",
        ck_body,
    )
    soa_i = ck_body.find("void run_on_dirty_blocks_only(IRModuleV2& mod)")
    soa_win = ck_body[soa_i : soa_i + 900] if soa_i >= 0 else ""
    must("for_each_block", "AC4 CK SoA peel", soa_win)
    must("dirty_only=*/true", "AC4 CK dirty_only", soa_win)
    if "run(module)" in soa_win or "run(mod, /*dirty" in soa_win:
        fails.append("AC4: CK SoA peel must not call full run(IRModule&) on clean")

    cf = impls.find("export class ConstantFoldingWrap")
    cf_end = impls.find("static_assert(DirtyAwarePass<ConstantFoldingWrap>", cf) if cf >= 0 else -1
    cf_body = impls[cf:cf_end] if cf >= 0 and cf_end > cf else ""
    must("void run_on_dirty_blocks_only(IRModuleV2& mod)", "AC1 CF SoA entry", cf_body)
    must(
        "void run_on_dirty_blocks_only(aura::ir::IRFunction& func, BlockDirtyPred pred = {})",
        "AC3 CF AoS kept",
        cf_body,
    )

    must("check_production_pure_wrap_pack", "AC2 consteval pack", core)
    must("run_production_soa_pure_wrap_pack", "AC2 runtime pack", core)
    pack = core.find("bool run_production_soa_pure_wrap_pack(IRModuleV2& mod, Passes&... passes) {")
    pwin = core[max(0, pack - 200) : pack + 500] if pack >= 0 else ""
    must("ProductionPureWrapPass", "AC2 constrained", pwin)
    must("run_on_dirty_blocks_only(mod)", "AC2 SoA dispatch", pwin)
    must("AoS-only wrap fails to instantiate", "AC2 compile-fail fixture", impls)
    must("run_production_soa_pure_wrap_pack(mod, ck, cf, tp, sh)", "AC2 hot pack uses PureWrap", impls)

    must("!prod_soa", "AC3 Soft AoS suite kept", svc)
    must("!ProductionPureWrapPass<EscapeAnalysisWrap>", "AC3 Escape grandfather", svc)
    must("DirtySoAEntryPass<EscapeAnalysisWrap>", "AC3 Escape DirtySoAEntry", svc)

    must("3488 AC1: CK ProductionPureWrapPass", "AC5 live AC1", t)
    must("3488 AC4: clean blocks skipped (no full-function AoS rebuild)", "AC5 live AC4", t)
    must("3488", "AC5 contracts suite cite", opt)
    must("check_production_pure_wrap_soa_3454", "AC5 #3454 linter stays", l3454)
    must("check_production_pipeline_purity_3329", "AC5 #3329 linter stays", l3329)
    must("static_assert(ProductionPureWrapPass<ComputeKindWrap>", "AC5 #3454 CK updated", l3454)

    must("check_production_pure_wrap_hot_pack_3488", "AC5 build.py", build)
    prev = build.find("check_production_pure_wrap_soa_3454")
    ours = build.find("check_production_pure_wrap_hot_pack_3488")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3454")

    must_not("schema-3488", "AC5 no schema-3488", impls)
    must_not("schema-3488", "AC5 no schema in service", svc)
    must_not("g_3488_", "AC5 no g_3488_*", impls)
    if (ROOT / "tests" / "compiler" / "test_issue_3488.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3488.cpp present")
    if (ROOT / "tests" / "issues" / "test_issue_3488.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3488.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3488-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    if fails:
        print("FAIL #3488 production_pure_wrap_hot_pack:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3488 production_pure_wrap_hot_pack: SoA PureWrap pack; AoS Soft kept")
    return 0


if __name__ == "__main__":
    sys.exit(main())
