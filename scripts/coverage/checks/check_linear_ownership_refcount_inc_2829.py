#!/usr/bin/env python3
"""Issue #2829: RefCountOp-inc is non-consuming; only dec marks moved.

Contract (one row per AC):
  AC1 is_consuming takes IRInstruction; operands[2] for RefCountOp; #2829
  AC2 refcount_inc_false_positive metric + note path
  AC3 test suite present (inc clean / dec UaM)
  AC4 linter wired; schema-2829; no docs/design/2829-*; no test_issue_2829.cpp

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


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    impls = _read("src/compiler/pass_impls.ixx")
    ir = _read("src/compiler/ir.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_linear_ownership_refcount_inc.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("Issue #2829", "AC1", impls)
    must("operands[2]", "AC1", impls)
    must("is_consuming_(const aura::ir::IRInstruction", "AC1", impls)
    must("is_consuming(const aura::ir::IRInstruction", "AC1", impls)
    # Residual always-true on bare RefCountOp in is_consuming switch is banned.
    # Look for old pattern: case RefCountOp: // dec variant + return true without ops[2]
    bad = re.search(
        r"case\s+aura::ir::IROpcode::RefCountOp\s*:\s*//\s*dec variant\s*\n\s*return\s+true",
        impls,
    )
    if bad:
        fails.append("AC1: residual always-true RefCountOp '// dec variant' path")
    must("inc(1)/dec(0)", "AC1", ir)

    # AC2
    must("refcount_inc_false_positive", "AC2", impls)
    must("note_refcount_inc_non_consume", "AC2", impls)

    # AC3
    must("ac2829", "AC3", test)
    must("2829", "AC3", test)
    must("RefCountOp", "AC3", test)
    must("use_after_move_count", "AC3", test)
    must("flag=*/1", "AC3", test)
    must("flag=*/0", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_linear_ownership_refcount_inc.cpp").is_file():
        fails.append("AC3: missing test_linear_ownership_refcount_inc.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2829.cpp").is_file():
        fails.append("AC3: test_issue_2829.cpp present (forbidden per #81967)")
    must("test_linear_ownership_refcount_inc", "AC3", cmake)

    # AC4
    must("check_linear_ownership_refcount_inc_2829", "AC4", build)
    must("schema-2829", "AC4", obs)
    must("linear-ownership-refcount-inc-false-positive-total", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2829-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2829 LinearOwnership RefCountOp-inc non-consume")
    return 0


if __name__ == "__main__":
    sys.exit(main())
