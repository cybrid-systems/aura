#!/usr/bin/env python3
"""Issue #2834: DCEPass skips Branch/Jump block-id operands in used-set scan.

Contract (one row per AC):
  AC1 mark_used_slots Branch/Jump cases; Issue #2834; overprotect metric
  AC2 Branch marks only cond slot (not targets)
  AC3 test suite present
  AC4 linter wired; schema-2834; no docs/design/2834-*; no test_issue_2834.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_dce_branch_block_id_overprotect.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("Issue #2834", "AC1", impls)
    must("branch_block_id_overprotect", "AC1", impls)
    pos = impls.find("static void mark_used_slots")
    body = impls[pos : pos + 2500] if pos >= 0 else ""
    must("case O::Branch:", "AC1", body)
    must("case O::Jump:", "AC1", body)

    # AC2 — Branch only marks operands[0]
    br = body.find("case O::Branch:")
    br_body = body[br : br + 500] if br >= 0 else ""
    must("operands[0]", "AC2", br_body)
    # Must not mark operands[1] as a slot use (only overprotect counter).
    if "mark(instr.operands[1]" in br_body or "mark(instr.operands[2]" in br_body:
        fails.append("AC2: Branch still mark()s block-id operands as slots")

    # AC3
    must("ac2834", "AC3", test)
    must("2834", "AC3", test)
    must("IROpcode::Branch", "AC3", test)
    must("IROpcode::Jump", "AC3", test)
    must("DCEPass", "AC3", test)
    must("eliminated_count", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_dce_branch_block_id_overprotect.cpp").is_file():
        fails.append("AC3: missing test_dce_branch_block_id_overprotect.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2834.cpp").is_file():
        fails.append("AC3: test_issue_2834.cpp present (forbidden per #81967)")
    must("test_dce_branch_block_id_overprotect", "AC3", cmake)

    # AC4
    must("check_dce_branch_block_id_overprotect_2834", "AC4", build)
    must("schema-2834", "AC4", obs)
    must("dce-branch-block-id-overprotect-total", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2834-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2834 DCEPass Branch/Jump block-id not slot uses")
    return 0


if __name__ == "__main__":
    sys.exit(main())
