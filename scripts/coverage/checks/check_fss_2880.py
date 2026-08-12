#!/usr/bin/env python3
"""Issue #2880: std/fss fish school search backend.

Contract:
  AC1 swarm.aura: swarm-step-fss! feeding/collective/volitive + step opt
  AC2 fss.aura surface + docs/stdlib/fss.md vs PSO/ABC
  AC3 suite fss_2880.aura; INDEX entry
  AC4 linter wired in build.py
  AC5 no docs/design/2880-* / no test_issue_2880.cpp

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
    fss = _read("lib/std/fss.aura")
    docs = _read("docs/stdlib/fss.md")
    suite = _read("tests/suite/fss_2880.aura")
    index = _read("lib/std/INDEX.aura")
    build = _read("build.py")

    # AC1
    must("swarm-step-fss!", "AC1", swarm)
    must("feeding", "AC1", swarm)
    must("volitive", "AC1", swarm)
    must("barycenter", "AC1", swarm)
    must("*swarm-fss-step*", "AC1", swarm)
    must("#2880", "AC1", swarm)
    must('"fss"', "AC1", swarm)

    # AC2
    must("fss:init", "AC2", fss)
    must("fss:volitive", "AC2", fss)
    must("#2880", "AC2", fss)
    if not docs:
        fails.append("AC2: docs/stdlib/fss.md missing")
    else:
        must("PSO", "AC2", docs)
        must("Volitive", "AC2", docs)
        must("Feeding", "AC2", docs)

    # AC3
    must("OK-2880", "AC3", suite)
    must('"fss"', "AC3", index)

    # AC4
    must("check_fss_2880", "AC4", build)

    # AC5
    if (ROOT / "tests" / "compiler" / "test_issue_2880.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_2880.cpp present (forbidden per #81967)")
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2880-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2880 fss — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
