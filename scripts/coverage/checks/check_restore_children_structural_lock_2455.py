#!/usr/bin/env python3
"""Issue #2455: restore_children acquires structural exclusive lock.

Contract:
  AC1 restore_children takes StructuralMutationGuard + contract_assert
  AC2 restore_children_locked exists for nested locked scopes
  AC3 test + gate wiring

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

    ast = _read("src/core/ast.ixx")
    test = _read("tests/core/test_restore_children_structural_lock.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Locate restore_children public wrapper body
    idx = ast.find("void restore_children(std::vector<PersistentChildVector<NodeId>>&& snapshot)")
    if idx < 0:
        fails.append("AC1: restore_children not found")
        body = ""
    else:
        body = ast[idx : idx + 900]

    must("Issue #2455", "AC1", ast)
    must("StructuralMutationGuard guard(this)", "AC1", body)
    must("contract_assert(static_cast<bool>(guard))", "AC1", body)
    must("2455 AC1", "AC1", test)

    must("restore_children_locked", "AC2", ast)
    must("2455 AC2", "AC2", test)

    must("2455 AC3", "AC3", test)
    must("check_restore_children_structural_lock_2455", "gate", build)
    must("cmd_restore_children_structural_lock_coverage", "gate", build)
    must("test_restore_children_structural_lock", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: restore_children structural lock #2455 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
