#!/usr/bin/env python3
"""Issue #2453: get(NodeId) NodeView multi-column snapshot contract.

Contract:
  AC1 FlatAST documents get() torn-read / post-parse contract (Issue #2453)
  AC2 get(NodeId) body cites multi-column snapshot + external serial
  AC3 concurrent multi-reader test + gate wiring

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
    test = _read("tests/core/test_get_nodeview_snapshot_2453.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2453", "AC1", ast)
    must("get(NodeId)", "AC1", ast)
    must("NodeView", "AC1", ast)
    must("torn", "AC1", ast)
    must("2453 AC1", "AC1", test)

    must("PRIMARY reader", "AC2", ast)
    must("workspace_mtx", "AC2", ast)
    must("2453 AC2", "AC2", test)
    must("concurrent multi-reader", "AC2", test)

    must("2453 AC3", "AC3", test)
    must("check_get_nodeview_snapshot_2453", "gate", build)
    must("cmd_get_nodeview_snapshot_coverage", "gate", build)
    must("test_get_nodeview_snapshot_2453", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: get NodeView snapshot #2453 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
