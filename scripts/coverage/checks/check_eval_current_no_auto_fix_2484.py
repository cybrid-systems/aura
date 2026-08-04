#!/usr/bin/env python3
"""Issue #2484: eval-current must not auto-invoke workspace Defines.

Contract:
  AC1 #2484 cite + REMOVED documentation
  AC2 no winning_call / auto_fixed / arg_pats / hardcoded probe lists
  AC3 gate wiring

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    src = _read("src/compiler/evaluator_primitives_eval.cpp")
    test = _read("tests/compiler/test_eval_current_no_auto_fix.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Scope to eval-current registration body (through eval-current-output)
    eidx = src.find("kEvalCurrent")
    if eidx < 0:
        eidx = src.find("eval-current")
    eend = src.find("kEvalCurrentOutput", eidx)
    if eidx >= 0 and eend > eidx:
        body = src[eidx:eend]
    elif eidx >= 0:
        body = src[eidx : eidx + 5000]
    else:
        body = src

    must("Issue #2484", "AC1", body)
    must("REMOVED", "AC1", body)

    must_not("winning_call", "AC2", body)
    must_not("auto_fixed", "AC2", body)
    must_not("arg_pats", "AC2", body)
    must_not("list 3 1 4 1 5", "AC2", body)
    must_not("dup2(null_fd", "AC2", body)

    must("2484 AC1", "gate", test)
    must("check_eval_current_no_auto_fix_2484", "gate", build)
    must("cmd_eval_current_no_auto_fix_coverage", "gate", build)
    must("test_eval_current_no_auto_fix", "gate", cmake)
    must("2484 AC4", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: eval-current no auto-fix #2484 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
