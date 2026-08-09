#!/usr/bin/env python3
"""Issue #2828: LinearOwnership input scan covers Branch/Return/CellGet/MakePair.

Contract (one row per AC):
  AC1 reads_input no longer false-lists Branch/Return/CellGet/MakePair;
      input_slot_range + Issue #2828 cites
  AC2 input_scan_missed metric + note path
  AC3 test suite present (Branch/Return/CellGet/MakePair UaM)
  AC4 linter wired; schema-2828; no docs/design/2828-*; no test_issue_2828.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _false_list_includes(body: str, op: str) -> bool:
    """True if op appears as a case arm that returns false in this switch body."""
    # Rough window: from function start to closing of switch default.
    return bool(
        re.search(
            rf"case\s+aura::ir::IROpcode::{op}\s*:"
            rf"(?:(?!case\s+aura::ir::IROpcode::)(?!default\s*:).)*?"
            rf"return\s+false",
            body,
            re.DOTALL,
        )
    )


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    impls = _read("src/compiler/pass_impls.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_linear_ownership_branch_cellget.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1: Wrap + Pass reads_input fixed
    wrap = impls.find("static bool reads_input_(aura::ir::IROpcode op)")
    wrap_body = impls[wrap : wrap + 600] if wrap >= 0 else ""
    pass_pos = impls.rfind("static bool reads_input(aura::ir::IROpcode op)")
    pass_body = impls[pass_pos : pass_pos + 600] if pass_pos >= 0 else ""

    must("Issue #2828", "AC1", impls)
    must("input_slot_range", "AC1", impls)
    for op in ("Branch", "Return", "CellGet", "MakePair"):
        if _false_list_includes(wrap_body, op):
            fails.append(f"AC1: Wrap reads_input_ still false-lists {op}")
        if _false_list_includes(pass_body, op):
            fails.append(f"AC1: Pass reads_input still false-lists {op}")
    must("case O::Branch:", "AC1", impls)
    must("case O::Return:", "AC1", impls)
    # Nop/Jump/ConstVoid remain the only false cases in both.
    must("IROpcode::Jump", "AC1", wrap_body)
    must("IROpcode::ConstVoid", "AC1", wrap_body)

    # AC2
    must("input_scan_missed", "AC2", impls)
    must("note_input_scan_missed", "AC2", impls)
    must("is_recovered_input_op", "AC2", impls)

    # AC3
    must("ac2828", "AC3", test)
    must("2828", "AC3", test)
    must("IROpcode::Branch", "AC3", test)
    must("IROpcode::Return", "AC3", test)
    must("IROpcode::CellGet", "AC3", test)
    must("IROpcode::MakePair", "AC3", test)
    must("use_after_move_count", "AC3", test)
    must("LinearOwnershipWrap", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_linear_ownership_branch_cellget.cpp").is_file():
        fails.append("AC3: missing test_linear_ownership_branch_cellget.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2828.cpp").is_file():
        fails.append("AC3: test_issue_2828.cpp present (forbidden per #81967)")
    must("test_linear_ownership_branch_cellget", "AC3", cmake)

    # AC4
    must("check_linear_ownership_branch_cellget_2828", "AC4", build)
    must("schema-2828", "AC4", obs)
    must("linear-ownership-input-scan-missed-total", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2828-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2828 linear ownership Branch/Return/CellGet/MakePair input scan")
    return 0


if __name__ == "__main__":
    sys.exit(main())
