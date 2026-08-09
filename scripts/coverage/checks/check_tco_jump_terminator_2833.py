#!/usr/bin/env python3
"""Issue #2833: TCOPass emits single Jump terminator (no duplicate / Branch residue).

Contract (one row per AC):
  AC1 single Jump assign in transform; Issue #2833; no 'need Branch'
  AC2 design comment uses Jump terminator (not Branch)
  AC3 test suite present
  AC4 linter wired; schema-2833; no docs/design/2833-*; no test_issue_2833.cpp

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
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_tco_jump_terminator_emitted.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1 — transform window
    must("Issue #2833", "AC1", impls)
    must("kTcoTerminatorIsJump", "AC1", impls)
    must("kTcoJumpTerminatorIssue", "AC1", impls)
    pos = impls.find("Issue #2833: single Jump assignment")
    if pos < 0:
        fails.append("AC1: missing single Jump assignment stamp")
        body = ""
    else:
        body = impls[pos : pos + 600]
    assigns = body.count("opcode = aura::ir::IROpcode::Jump")
    if assigns != 1:
        fails.append(f"AC1: expected exactly 1 Jump assign in transform window, got {assigns}")
    if "need Branch" in body:
        fails.append("AC1: residual 'need Branch' comment")
    if "Branch is the new terminator" in body:
        fails.append("AC1: residual 'Branch is the new terminator'")
    must("Jump is the new terminator", "AC1", body)

    # AC2 — class design comment
    must("Replace the Call with a Jump", "AC2", impls)
    if "Remove the Return (the Branch is the new terminator)" in impls:
        fails.append("AC2: design comment still says Branch terminator")
    # Flag consecutive duplicate Jump assigns anywhere near TCO run_on_block.
    if re.search(
        r"opcode\s*=\s*aura::ir::IROpcode::Jump\s*;[^\n]*\n"
        r"(?:[^\n]*\n){0,4}"
        r"\s*call_instr->opcode\s*=\s*aura::ir::IROpcode::Jump\s*;",
        impls,
    ):
        fails.append("AC2: consecutive duplicate call_instr->opcode = Jump")

    # AC3
    must("ac2833", "AC3", test)
    must("2833", "AC3", test)
    must("IROpcode::Jump", "AC3", test)
    must("IROpcode::Branch", "AC3", test)  # assert zero Branch
    must("TCOPass", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_tco_jump_terminator_emitted.cpp").is_file():
        fails.append("AC3: missing test_tco_jump_terminator_emitted.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2833.cpp").is_file():
        fails.append("AC3: test_issue_2833.cpp present (forbidden per #81967)")
    must("test_tco_jump_terminator_emitted", "AC3", cmake)

    # AC4
    must("check_tco_jump_terminator_2833", "AC4", build)
    must("schema-2833", "AC4", obs)
    must("tco-jump-terminator-wired", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2833-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2833 TCOPass single Jump terminator — no Branch residue")
    return 0


if __name__ == "__main__":
    sys.exit(main())
