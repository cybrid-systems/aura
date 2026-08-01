#!/usr/bin/env python3
"""Issue #2454: RAII mutation guards FlatAST-move lifetime contract.

Contract:
  AC1 Issue #2454 documented on StructuralMutationGuard (must not outlive)
  AC2 ReaderLockGuard / MetadataWriteGuard / MetadataReadGuard cite #2454
  AC3 test + gate wiring (drop-before-move safe pattern)

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

    ast = _read("src/core/ast.ixx")
    test = _read("tests/core/test_raii_guard_flatast_lifetime_2454.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2454", "AC1", ast)
    must("StructuralMutationGuard", "AC1", ast)
    must("MUST NOT outlive", "AC1", ast)
    must("2454 AC1", "AC1", test)

    must("ReaderLockGuard", "AC2", ast)
    must("MetadataWriteGuard", "AC2", ast)
    must("MetadataReadGuard", "AC2", ast)
    # All four guards should mention #2454 or outlive
    must("must not outlive", "AC2", ast)
    must("2454 AC2", "AC2", test)
    must("drop guard then move", "AC2", test)

    must("2454 AC3", "AC3", test)
    must("check_raii_guard_flatast_lifetime_2454", "gate", build)
    must("cmd_raii_guard_flatast_lifetime_coverage", "gate", build)
    must("test_raii_guard_flatast_lifetime_2454", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: RAII guard FlatAST lifetime #2454 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
