#!/usr/bin/env python3
"""Issue #4262: run_dirty_escape_on_soa zero FlatInstruction materialize.

#3701/#3795 made EscapeAnalysisWrap ProductionPureWrap + SoA dirty entry.
Residual: run_dirty_escape_on_soa still copied dirty SoA columns into
AoS FlatInstruction vectors. Fix: columnar run_escape_on_soa_fn peels
in place. Soft AoS grandfather unchanged.

Contract:
  AC1  run_dirty_escape_on_soa has no FlatInstruction; uses run_escape_on_soa_fn
  AC2  escape maps still update; shape-stable skip unchanged (#3701)
  AC3  Soft AoS EscapeAnalysisWrap::run(IRFunction&) + FlatInstruction kept
  AC4  extends test_soa_dirty_aware_pipeline; linter + gf + manifest + build.py
       no invent / docs/design / schema-4262

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SVC = "src/compiler/service.ixx"
TEST = "tests/compiler/test_soa_dirty_aware_pipeline.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
MANIFEST = "scripts/coverage/manifests/4262.json"
LINTER = "check_escape_soa_no_flat_4262"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    svc = _read(SVC)
    test = _read(TEST)
    build = _read(BUILD)
    gf = _read(GF)
    man = _read(MANIFEST)

    must("Issue #4262", "AC1 cite", svc)
    must("kEscapeSoaNoFlatMaterializeIssue = 4262", "AC1 stamp", svc)
    must("run_escape_on_soa_fn", "AC1 helper", svc)
    must("run_dirty_escape_on_soa", "AC1 entry", svc)

    # Definition that calls run_escape_on_soa_fn must not construct FlatInstruction.
    helper_call = svc.find("detail_escape_soa::run_escape_on_soa_fn")
    if helper_call < 0:
        fails.append("AC1: must call detail_escape_soa::run_escape_on_soa_fn")
    else:
        # Window around the production dirty entry definition
        win_start = svc.rfind("export inline std::size_t run_dirty_escape_on_soa", 0, helper_call)
        win = svc[win_start:helper_call + 80] if win_start >= 0 else svc[helper_call - 800:helper_call + 80]
        win_code = re.sub(r"//[^\n]*", "", win)
        if "FlatInstruction" in win_code:
            fails.append("AC1: run_dirty_escape_on_soa still constructs FlatInstruction")
        if "std::vector<std::vector<" in win_code and "Flat" in win_code:
            fails.append("AC1: residual AoS vector materialize")

    must("g_fn_shape_stable_probe", "AC2 shape-stable", svc)
    must("DirtySoAEntryPass<EscapeAnalysisWrap>", "AC3 Soft DirtySoAEntry", svc)
    must("void run(aura::ir::IRFunction& func)", "AC3 Soft AoS run", svc)
    must("aura::jit::run_escape_analysis(flat_instrs", "AC3 Soft Flat path", svc)

    must("4262 AC1", "AC4 test", test)
    must("4262 AC3", "AC4 test Soft", test)
    must(LINTER, "AC4 build", build)
    must("Issue #4262", "AC4 build cite", build)
    must(LINTER + ".py", "AC4 gf", gf)
    must('"issue": 4262', "AC4 manifest", man)

    if "schema-4262" in svc:
        fails.append("AC4: schema-4262 invent forbidden")
    if (ROOT / "tests/compiler/test_issue_4262.cpp").is_file():
        fails.append("AC4: test_issue_4262.cpp invent forbidden")
    if (ROOT / "docs/design/4262-escape-soa-no-flat.md").is_file():
        fails.append("AC4: docs/design invent forbidden")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: #4262 escape SoA zero FlatInstruction materialize — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
