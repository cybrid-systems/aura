#!/usr/bin/env python3
"""Issue #2875: std/swarm common population/step/best API + grid baseline.

Contract:
  AC1 swarm:population + mean-fit in report/export; discrete grid builder
  AC2 docs/stdlib/swarm.md present
  AC3 suite swarm_interface_2875.aura
  AC4 linter wired in build.py
  AC5 no docs/design/2875-* / no test_issue_2875.cpp

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

    swarm = _read("lib/std/swarm.aura")
    docs = _read("docs/stdlib/swarm.md")
    suite = _read("tests/suite/swarm_interface_2875.aura")
    build = _read("build.py")

    # AC1
    must("#2875", "AC1", swarm)
    must("swarm:population", "AC1", swarm)
    must("mean-fit", "AC1", swarm)
    must("swarm-build-grid-full", "AC1", swarm)
    must("swarm-grid-window", "AC1", swarm)
    must("discrete linspace", "AC1", swarm)

    # AC2
    if not docs:
        fails.append("AC2: docs/stdlib/swarm.md missing")
    else:
        must("swarm:population", "AC2", docs)
        must("mean-fit", "AC2", docs)
        must('kind: "grid"', "AC2", docs)
        must("2875", "AC2", docs)

    # AC3
    must("OK-2875", "AC3", suite)
    must("swarm:population", "AC3", suite)
    must("mean-fit", "AC3", suite)

    # AC4
    must("check_swarm_interface_2875", "AC4", build)

    # AC5
    if (ROOT / "tests" / "compiler" / "test_issue_2875.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_2875.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2875-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2875 swarm interface — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
