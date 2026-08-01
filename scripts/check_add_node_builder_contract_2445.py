#!/usr/bin/env python3
"""Issue #2445: add_node + add_* builder single-threaded mutation contract.

Contract:
  AC1 FlatAST documents builder mutation contract (Issue #2445)
  AC2 add_node still holds flatast_mutex_ (#2413 / parallel-ready size path)
  AC3 test covers single-thread builders + concurrent add_node + gate wiring

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
    test = _read("tests/core/test_add_node_builder_contract_2445.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2445", "AC1", ast)
    must("add_* builder", "AC1", ast)
    must("single-threaded mutation", "AC1", ast)
    must("workspace_mtx", "AC1", ast)
    must("add_literal", "AC1", ast)
    must("2445 AC1", "AC1", test)

    must("flatast_mutex_", "AC2", ast)
    # #2488: exclusive unique_lock on OwnedSharedMutex (was recursive_mutex).
    if "unique_lock" not in ast or "flatast_mutex_" not in ast:
        fails.append("AC2: add_node must exclusive-lock flatast_mutex_")
    else:
        # Accept exclusive SoA write under flatast_mutex_ (#2413 / #2488).
        must("flatast_mutex_.mutable_get()", "AC2", ast)
    must("2445 AC2", "AC2", test)
    must("concurrent add_node", "AC2", test)

    must("2445 AC3", "AC3", test)
    must("check_add_node_builder_contract_2445", "gate", build)
    must("cmd_add_node_builder_contract_coverage", "gate", build)
    must("test_add_node_builder_contract_2445", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: add_node builder contract #2445 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
