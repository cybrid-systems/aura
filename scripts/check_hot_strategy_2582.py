#!/usr/bin/env python3
"""Issue #2582: pure-Aura hot strategy contracts.

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

    hs = _read("lib/std/hot-strategy.aura")
    hu = _read("lib/std/hot-update.aura")
    idx = _read("lib/std/INDEX.aura")
    doc = _read("docs/stdlib/hot-strategy.md")
    test = _read("tests/compiler/test_hot_strategy_2582.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2582", "AC1", hs)
    must("hot-strategy:swap!", "AC1", hs)
    must("hot-strategy:heal!", "AC1", hs)
    must("mutate:rebind", "AC1", hs)
    must("ast:snapshot", "AC1", hs)
    must("#2582", "AC2", hu)
    must("hot-strategy", "AC2", hu)
    must("AOT", "AC2", hu)
    must("hot-strategy", "AC3", idx)
    must("#2582", "AC3", doc)
    must("test_hot_strategy_2582", "AC4", cmake)
    must("check_hot_strategy_2582", "AC4", build)
    must("ac1_swap", "AC4", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2582 pure-Aura hot strategy — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
