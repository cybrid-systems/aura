#!/usr/bin/env python3
"""Issue #3314: extend append-only offsetof/sizeof stamps beyond PcvHotpathMetrics.

#3292 pinned PcvHotpathMetrics. The same BMI-offset failure mode still
exists for other hot SoA / metrics structs that Agents grow by mid-struct
insertion. This linter enumerates the stamped structs and fails CI if a
stamp is missing or an offset drifts without a deliberate stamp update.

Contract:
  AC1  IR SoA dirty/column tail (IRFunctionSoA) + IRInstructionView +
       at least one additional hot struct (DensifyConsistencyReport,
       LayoutStamp) carry offsetof/sizeof stamps.
  AC2  stamps present, offsets match, sizeof match; no g_3314_* runtime.
  AC3  Soft/Off/unit: compile-time only (static_assert).
  AC4  extend test_ir_soa_layout_stamp; no test_issue_3314.cpp; no docs/design.

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

    soa = _read("src/compiler/ir_soa.ixx")
    dens = _read("src/core/densify_consistency_report.h")
    ls = _read("src/core/layout_stamp.hh")
    pcv = _read("src/core/persistent_child_vector.hh")
    test = _read("tests/compiler/test_ir_soa_layout_stamp.cpp")
    build = _read("build.py")
    lint3292 = _read("scripts/coverage/checks/check_pcv_hotpath_metrics_layout_3292.py")

    # ── AC1: IR SoA dirty/column + additional hot structs ──
    must("Issue #3314", "AC1 ir_soa cite", soa)
    must("kAppendOnlyLayoutStampIssue = 3314", "AC1 issue constant", soa)
    must("offsetof(IRFunctionSoA, block_dirty_) == 376", "AC1 block_dirty offset", soa)
    must("offsetof(IRFunctionSoA, instruction_dirty_) == 408", "AC1 instruction_dirty offset", soa)
    must("offsetof(IRFunctionSoA, generation_) == 440", "AC1 generation offset", soa)
    must("sizeof(IRFunctionSoA) == 448", "AC1 IRFunctionSoA sizeof", soa)
    must("offsetof(IRInstructionView, idx) == 8", "AC1 view idx offset", soa)
    must("sizeof(IRInstructionView) == 16", "AC1 view sizeof", soa)
    # Stamps sit after BasicBlockSoA (IRFunctionSoA complete).
    bb = soa.find("export struct BasicBlockSoA")
    gen_stamp = soa.find("offsetof(IRFunctionSoA, generation_) == 440")
    if bb < 0 or gen_stamp < 0 or gen_stamp < bb:
        fails.append("AC1: IRFunctionSoA stamps must sit after BasicBlockSoA (complete type)")

    must("Issue #3314", "AC1 densify cite", dens)
    must("offsetof(DensifyConsistencyReport, envframe_ok) == 7", "AC1 densify last field", dens)
    must("sizeof(DensifyConsistencyReport) == 8", "AC1 densify sizeof", dens)
    struct_d = dens.find("struct DensifyConsistencyReport")
    stamp_d = dens.find("offsetof(DensifyConsistencyReport, envframe_ok)")
    if struct_d < 0 or stamp_d < 0 or stamp_d < struct_d:
        fails.append("AC1: densify stamps must sit after DensifyConsistencyReport")

    must("Issue #3314", "AC1 LayoutStamp cite", ls)
    must("offsetof(LayoutStamp, ir_soa_generation) == 56", "AC1 LayoutStamp last field", ls)
    must("sizeof(LayoutStamp) == 64", "AC1 LayoutStamp sizeof", ls)

    # 3292 PCV stamps remain the pattern / lineage.
    must("sizeof(PcvHotpathMetrics) == 136", "AC1 PCV 3292 lineage intact", pcv)
    must("3292", "AC1 3292 linter lineage", lint3292)

    # ── AC2: no new runtime counters ──
    for name, hay in (("ir_soa", soa), ("densify", dens), ("layout_stamp", ls)):
        if "g_3314_" in hay:
            fails.append(f"AC2: {name} invented g_3314_* runtime counter")
    must("static_assert(offsetof", "AC2 compile-time offsetof", soa)
    must("3314 AC2", "AC2 test", test)

    # ── AC3: compile-time only ──
    must("Compile-time only", "AC3 densify zero runtime", dens)
    must("compile-time only", "AC3 ir_soa zero runtime", soa.lower())
    must("3314 AC3", "AC3 test", test)

    # ── AC4: existing suite + linter after #3292; no invent / docs ──
    must("check_append_only_layout_stamps_3314", "AC4 build.py", build)
    must("3314 AC1", "AC4 test AC1", test)
    must("3314 AC4", "AC4 test", test)
    prev = build.find("check_pcv_hotpath_metrics_layout_3292")
    ours = build.find("check_append_only_layout_stamps_3314")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: linter must be wired in build.py AFTER #3292")
    if (ROOT / "tests" / "compiler" / "test_issue_3314.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3314.cpp per #81967")
    if (ROOT / "tests" / "core" / "test_issue_3314.cpp").is_file():
        fails.append("AC4: forbidden tests/core/test_issue_3314.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3314-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3314 append_only_layout_stamps:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3314 append_only_layout_stamps: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
