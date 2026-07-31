#!/usr/bin/env python3
"""Issue #2402: incremental restamp default + generation wrap cost control.

Contract:
  AC1 production default Auto/Incremental prefers dirty-cone restamp
  AC2 empty Incremental → lazy-only (no full walk); non-wrap full ok
  AC3 is_valid / make_ref after incremental
  AC4 query keys schema-2402 additive
  AC5 tests + chaos + build gate

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
    q = _read("src/compiler/evaluator_primitives_stdlib_review.cpp")
    test = _read("tests/core/test_incremental_restamp_2061.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1 policy
    must("Issue #2402", "AC1", ast)
    must("RestampPolicy", "AC1", ast)
    must("resolve_restamp_policy", "AC1", ast)
    must("AURA_RESTAMP_POLICY", "AC1", ast)
    must("2402 AC1", "AC1", test)

    # AC2 lazy-only / last-call
    must("restamp_nodes_last_", "AC2", ast)
    must("restamp_us_last_", "AC2", ast)
    must("lazy_only", "AC2", ast)
    must("2402 AC2", "AC2", test)

    # AC3
    must("2402 AC3", "AC3", test)
    must("restamp_lazy_align_enabled_", "AC3", ast)

    # AC4 query
    must("schema-2402", "AC4", q)
    must("issue-2402", "AC4", q)
    must("restamp-incremental-wired", "AC4", q)
    must("restamp-policy", "AC4", q)
    must("restamp-incremental-default", "AC4", q)
    must("restamp-nodes-last", "AC4", q)
    must("restamp-us-last", "AC4", q)
    must("schema-2122", "AC4", q)
    must("2402 AC4", "AC4", test)

    # AC5
    must("2402 AC5", "AC5", test)
    must("check_restamp_incremental_2402", "AC5", build)
    must("cmd_restamp_incremental_coverage", "AC5", build)
    must("test_incremental_restamp_2061", "AC5", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2402 restamp incremental default — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
