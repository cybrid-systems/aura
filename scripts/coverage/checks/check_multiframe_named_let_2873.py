#!/usr/bin/env python3
"""Issue #2873: multi-frame named-let past early depth caps + prim shadow.

#2871 made TW named-let true TCO (continue, not return eval_flat). #2873
locks the quantitative multi-frame case (string-append+cons / cache-put)
past former ~200 / ~120 thresholds, and fixes Call prim fast-path so
env-bound names (let take / map / drop) shadow list primitives.

Contract:
  AC1 Call Variable: env binding shadows prim (#2873 comment + is_primitive)
  AC2 #2871 TCO continue for TW closure still present
  AC3 suite multiframe_named_let_2873.aura
  AC4 linter wired in build.py
  AC5 no docs/design/2873-* / no test_issue_2873.cpp

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

    ev = _read("src/compiler/evaluator_eval_flat.cpp")
    suite = _read("tests/suite/multiframe_named_let_2873.aura")
    build = _read("build.py")

    # AC1 — prim shadow
    must("#2873", "AC1", ev)
    must("only take the prim fast-path when the name is", "AC1", ev)
    must("is_primitive(*bound)", "AC1", ev)
    must("let take", "AC1", ev)

    # AC2 — TCO still present
    must("true TCO for TW closure calls", "AC2", ev)
    must("current_id = cl.body_id", "AC2", ev)

    # AC3 — suite
    must("2873", "AC3", suite)
    must("deep-map", "AC3", suite)
    must("cache-put", "AC3", suite)
    must("OK-2873", "AC3", suite)
    must("let take", "AC3", suite)

    # AC4 — wire
    must("check_multiframe_named_let_2873", "AC4", build)

    # AC5 — forbidden
    if (ROOT / "tests" / "compiler" / "test_issue_2873.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_2873.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2873-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2873 multiframe named-let — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
