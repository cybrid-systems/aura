#!/usr/bin/env python3
"""Issue #2428: arm_defer uses fetch_or for first-arm metrics (no load+or race).

Contract:
  AC1 arm_defer uses fetch_or; arm sites do not separate load+note
  AC2 concurrent gate test
  AC3 note_defer_reason_armed called from arm_defer with prev from fetch_or
  AC4 nested depth semantics preserved; gate wired

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    hh = _read("src/core/gc_hooks.h")
    test = _read("tests/core/test_gc_defer_arm_fetch_or_2428.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2428", "AC1", hh)
    must("fetch_or", "AC1", hh)
    must("g_gc_defer_reasons.fetch_or", "AC1", hh)
    must("2428 AC1", "AC1", test)

    # No racy load-then-arm-then-note pattern remains in arm_* helpers.
    # Pattern: load reasons; arm_defer; note_defer
    if re.search(
        r"g_gc_defer_reasons\.load\([^)]*\);\s*\n\s*\(void\)arm_defer",
        hh,
    ):
        fails.append("AC1: racy load-before-arm_defer pattern still present")
    if re.search(
        r"arm_defer\([^)]*\);\s*\n\s*note_defer_reason_armed",
        hh,
    ):
        fails.append("AC1: separate note_defer_reason_armed after arm_defer still present")

    must("2428 AC2", "AC2", test)
    must("concurrent arm_ffi_pin_defer", "AC2", test)

    must("note_defer_reason_armed(r, prev)", "AC3", hh)
    must("2428 AC3", "AC3", test)
    must("first-arm metric +1 under concurrent race", "AC3", test)

    must("2428 AC4", "AC4", test)
    must("check_gc_defer_arm_fetch_or_2428", "gate", build)
    must("cmd_gc_defer_arm_fetch_or_coverage", "gate", build)
    must("test_gc_defer_arm_fetch_or_2428", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: gc defer arm fetch_or #2428 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
