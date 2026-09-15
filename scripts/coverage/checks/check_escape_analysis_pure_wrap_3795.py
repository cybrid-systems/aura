#!/usr/bin/env python3
"""Issue #3795: EscapeAnalysisWrap satisfies ProductionPureWrapPass (SoA dirty).

AC1: ProductionPureWrapPass<EscapeAnalysisWrap>; SoA run_on_dirty_blocks_only(IRModuleV2&)
AC2: Soft/unit DirtySoAEntryPass + AoS suite retained (!prod_soa)
AC3: production else arm still uses run_dirty_escape_on_soa (no AoS Wrap run)
AC4: no new query key / no invent docs

Usage:
  python3 scripts/coverage/checks/check_escape_analysis_pure_wrap_3795.py
  python3 scripts/coverage/checks/check_escape_analysis_pure_wrap_3795.py --self-test
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def must(needle: str, label: str, hay: str, fails: list[str]) -> None:
    if needle not in hay:
        fails.append(f"FAIL: {label}")


def main() -> int:
    service = (ROOT / "src/compiler/service.ixx").read_text()
    sig = (ROOT / "src/compiler/pass_soa_sig.hh").read_text()
    fails: list[str] = []

    must("ProductionPureWrapPass<EscapeAnalysisWrap>", "AC1 ProductionPureWrapPass", service, fails)
    must("void run_on_dirty_blocks_only(IRModuleV2& mod)", "AC1 SoA dirty entry", service, fails)
    must("DirtySoAEntryPass<EscapeAnalysisWrap>", "AC2 Soft DirtySoAEntry retained", service, fails)
    must("if (!prod_soa)", "AC2 Soft AoS suite gate", service, fails)
    must("run_dirty_escape_on_soa", "AC3 columnar production escape", service, fails)
    must("Issue #3795", "AC1 cites #3795", service, fails)
    must("kEscapeAnalysisProductionPureWrapIssue = 3795", "AC1 stamp", sig, fails)
    if "schema-3795" in service:
        fails.append("FAIL: AC4 no schema-3795 query key")
    if (ROOT / "docs/design/3795-escape-pure-wrap.md").exists():
        fails.append("FAIL: AC4 no docs/design invent")
    if (ROOT / "tests/compiler/test_issue_3795.cpp").exists():
        fails.append("FAIL: AC4 no invent test_issue_3795")

    # Production else must not AoS-run escape_pass
    prod = service.find("const bool prod_soa")
    if prod < 0:
        fails.append("FAIL: AC3 prod_soa missing")
    else:
        win = service[prod : prod + 3200]
        aos = win.find("if (!prod_soa)")
        else_pos = win.find("} else {", aos) if aos >= 0 else -1
        if aos < 0 or else_pos < 0:
            fails.append("FAIL: AC2/AC3 Soft/prod arms missing")
        else:
            aos_body = win[aos:else_pos]
            else_body = win[else_pos : else_pos + 500]
            if "escape_pass" not in aos_body:
                fails.append("FAIL: AC2 Soft still runs EscapeAnalysisWrap")
            if "run_dirty_escape_on_soa" not in else_body:
                fails.append("FAIL: AC3 production uses run_dirty_escape_on_soa")
            if "escape_pass" in else_body:
                fails.append("FAIL: AC3 production must not AoS-run escape_pass")

    if fails:
        for f in fails:
            print(f)
        return 1
    print("OK: #3795 EscapeAnalysisWrap ProductionPureWrapPass present")
    return 0


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        # Smoke: script runs against live tree.
        raise SystemExit(main())
    raise SystemExit(main())
