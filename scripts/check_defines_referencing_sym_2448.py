#!/usr/bin/env python3
"""Issue #2448: defines_referencing_sym skips only mutated Define by NodeId.

Contract:
  AC1 exclude_define parameter on defines_referencing_sym
  AC2 skip i == exclude_define (not v.sym_id == sym)
  AC3 service caller passes target; test + gate wiring

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    ast = _read("src/core/ast.ixx")
    svc = _read("src/compiler/service.ixx")
    test = _read("tests/core/test_defines_referencing_sym_2448.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Locate defines_referencing_sym body
    idx = ast.find("defines_referencing_sym(SymId sym")
    if idx < 0:
        fails.append("AC1: defines_referencing_sym not found")
        body = ""
    else:
        body = ast[idx : idx + 1200]

    must("Issue #2448", "AC1", ast)
    must("exclude_define", "AC1", body)
    must("NULL_NODE", "AC1", body)
    must("2448 AC1", "AC1", test)

    must("i == exclude_define", "AC2", body)
    # Old name-based skip must not remain in the function body
    must_not("if (v.sym_id == sym)", "AC2", body)
    must("2448 AC2", "AC2", test)

    must("defines_referencing_sym", "AC3", svc)
    must("Issue #2448", "AC3", svc)
    must("2448 AC3", "AC3", test)
    must("check_defines_referencing_sym_2448", "gate", build)
    must("cmd_defines_referencing_sym_coverage", "gate", build)
    must("test_defines_referencing_sym_2448", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: defines_referencing_sym #2448 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
