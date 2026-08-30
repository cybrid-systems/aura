#!/usr/bin/env python3
"""Issue #3454: ProductionPureWrapPass type-checks SoA dirty entry, not AoS.

#3405 required any run_on_dirty_blocks_only, including IRFunction&.
A new kPureWrap Wrap with only the AoS dirty entry satisfied the
production concept and walked block.instructions. SoA is now the
requires-clause. Grandfather CK/CF/TP/Shape/Escape stay DirtySoAEntry.
InlinePass SoA + dual-emit abort (#3403) unchanged. No new query key.

Contract:
  AC1 AoS-only kPureWrap does not satisfy ProductionPureWrapPass
  AC2 InlinePass SoA still production dispatch; dual-emit abort stays
  AC3 grandfather list explicit, length-capped at 5; no silent drop
  AC4 BlockDirtyPred trivially copyable / no std::function (#3042)
  AC5 no docs/design/3454-*; no test_issue_3454.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    concepts = _read("src/core/concept_constraints.ixx")
    sig = _read("src/compiler/pass_soa_sig.hh")
    service = _read("src/compiler/service.ixx")
    pipeline = _read("src/compiler/pass_pipeline_core.ixx")
    pass_impls = _read("src/compiler/pass_impls.ixx")
    soa_view = _read("src/compiler/soa_view.ixx")
    l3405 = _read("scripts/check_pure_wrap_dirty_entry_3405.py")
    l3403 = _read("scripts/check_inline_pass_soa_3403.py")
    t = _read("tests/core/test_arena_required_cover_no_value_only.cpp")
    build = _read("build.py")

    must("kProductionPureWrapSoaIssue = 3454", "AC1 stamp", concepts)
    must("kProductionPureWrapSoaIssue = 3454", "AC1 header stamp", sig)
    must("import aura.compiler.ir_soa", "AC1 ir_soa import", concepts)
    must('#include "compiler/pass_soa_sig.hh"', "AC1 pass_soa_sig include", concepts)
    must("AosOnlyPureWrapStub", "AC1 AoS-only stub", concepts)
    must("SoaDirtyPureWrapStub", "AC1 SoA stub", concepts)
    must("!ProductionPureWrapPass<pass_soa_detail::AosOnlyPureWrapStub>", "AC1 compile-reject AoS-only", concepts)
    must("ProductionPureWrapPass<pass_soa_detail::SoaDirtyPureWrapStub>", "AC1 compile-accept SoA", concepts)

    start_ppw = concepts.find("concept ProductionPureWrapPass")
    end_ppw = concepts.find("concept DirtySoAEntryPass", start_ppw) if start_ppw >= 0 else -1
    ppw_body = concepts[start_ppw:end_ppw] if start_ppw >= 0 and end_ppw > start_ppw else ""
    if not ppw_body:
        fails.append("AC1: ProductionPureWrapPass concept body not found")
    if "aura::ir::IRFunction&" in ppw_body:
        fails.append("AC1: ProductionPureWrapPass still type-checks AoS IRFunction&")
    if "IRFunctionSoA" not in ppw_body:
        fails.append("AC1: ProductionPureWrapPass does not type-check IRFunctionSoA")
    if "IRModuleV2" not in ppw_body:
        fails.append("AC1: ProductionPureWrapPass does not type-check IRModuleV2")

    must("void run_on_dirty_blocks_only(IRModuleV2& module,", "AC2 InlinePass SoA", pass_impls)
    must("hard_zero_dual_emit_bridge_in_production", "AC2 dual-emit gate", soa_view)
    must("do not add InlinePass here", "AC2 no AoS InlinePass dispatch", pipeline)
    must("InlinePass::run_on_dirty_blocks_only(IRModuleV2&)", "AC2 SoA pack cite", pass_impls)

    grandfather = [
        "ComputeKindWrap",
        "ConstantFoldingWrap",
        "TypePropagationPass",
        "ShapeWrap",
        "EscapeAnalysisWrap",
    ]
    must("Issue #3454 AC3 grandfather (length-capped 5)", "AC3 cite", service)
    for name in grandfather:
        must(name, "AC3 grandfather name", service)
    start = service.find("std::size_t run_incremental_dirty_pass_suite_")
    end = service.find("run_coercion_elim_on_function", start) if start >= 0 else -1
    suite = service[start:end] if start >= 0 and end > start else ""
    n_pipe = len(re.findall(r"run_production_incremental_dirty_pipeline\(\s*ir_mod,", suite))
    if n_pipe != 5:
        fails.append(f"AC3: production incremental pack length is {n_pipe}, cap is 5")
    must("!ProductionPureWrapPass<ComputeKindWrap>", "AC3 CK not ProductionPureWrap", pass_impls)
    must("!ProductionPureWrapPass<EscapeAnalysisWrap>", "AC3 Escape not ProductionPureWrap", service)
    must("DirtySoAEntryPass<EscapeAnalysisWrap>", "AC3 Escape stays DirtySoAEntry", service)

    must("kPureWrapNoStdFunctionDirtyIssue = 3042", "AC4 #3042 stamp", pipeline)
    if "std::function" in ppw_body:
        fails.append("AC4: ProductionPureWrapPass requires-clause mentions std::function")
    must("struct BlockDirtyPred", "AC4 BlockDirtyPred", pipeline)
    bd = pipeline.find("export struct BlockDirtyPred")
    bwin = pipeline[bd : bd + 500] if bd >= 0 else ""
    if "std::function" in bwin:
        fails.append("AC4: BlockDirtyPred is not trivially copyable / uses std::function")

    must("ac3454_production_pure_wrap_soa", "AC1 test", t)
    must("check_pure_wrap_dirty_entry_3405", "AC5 extend 3405", l3405)
    must("AC2/#3454", "AC5 extend 3403", l3403)
    must("check_production_pure_wrap_soa_3454", "AC5 build.py", build)
    if "schema-3454" in concepts or "schema-3454" in service or "schema-3454" in pass_impls:
        fails.append("AC4: new schema-3454 query key")
    if (ROOT / "tests" / "core" / "test_issue_3454.cpp").is_file():
        fails.append("AC5: forbidden tests/core/test_issue_3454.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3454.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3454.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir() and list(docs.glob("3454-*.md")):
        fails.append("AC5: forbidden docs/design/3454-*.md")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("PASS: #3454 ProductionPureWrapPass SoA dirty-entry contract satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
