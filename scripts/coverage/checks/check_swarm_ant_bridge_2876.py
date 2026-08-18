#!/usr/bin/env python3
"""Issue #2876: std/ant mutation-type ranking as swarm kind:ant.

Contract:
  AC1 ant.aura: evaporate!, types, rank; colony:search still exported
  AC2 swarm kind:ant uses pheromone:rank/update + ops/evaporate opts
  AC3 suite swarm_ant_bridge_2876.aura (suite-only; examples/ merged into tests/suite/ per aura philosophy #1655 / 2026-07-19 cleanup wave); docs/stdlib/ant.md + swarm.md
  AC4 linter wired in build.py
  AC5 no docs/design/2876-* / no test_issue_2876.cpp

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

    ant = _read("lib/std/ant.aura")
    swarm = _read("lib/std/swarm.aura")
    suite = _read("tests/suite/swarm_ant_bridge_2876.aura")
    docs_ant = _read("docs/stdlib/ant.md")
    docs_sw = _read("docs/stdlib/swarm.md")
    build = _read("build.py")

    # AC1
    must("pheromone:evaporate!", "AC1", ant)
    must("pheromone:types", "AC1", ant)
    must("colony:search", "AC1", ant)
    must("#2876", "AC1", ant)
    must("mutation-type", "AC1", ant)

    # AC2
    must("swarm-step-ant!", "AC2", swarm)
    must("pheromone:rank", "AC2", swarm)
    must("pheromone:update", "AC2", swarm)
    must("pheromone:evaporate!", "AC2", swarm)
    must("*swarm-ant-ops*", "AC2", swarm)
    must("ops", "AC2", swarm)
    must("evaporate", "AC2", swarm)
    must("#2876", "AC2", swarm)

    # AC3
    must("OK-2876", "AC3", suite)
    must("colony:search", "AC3", suite)
    # example file removed in #1655 cleanup wave 9 (folded into suite)
    must("#2876", "AC3", docs_ant)
    must("mutation-type", "AC3", docs_sw)

    # AC4
    must("check_swarm_ant_bridge_2876", "AC4", build)

    # AC5
    if (ROOT / "tests" / "compiler" / "test_issue_2876.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_2876.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2876-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2876 swarm ant bridge — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
