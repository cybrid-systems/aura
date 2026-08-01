#!/usr/bin/env python3
"""Issue #2486: gc-heap fallback clears ev.cells_.

Contract:
  AC1 cells_.clear + shrink_to_fit in gc-heap fallback
  AC2 #2486 cite + audit comment
  AC3 other heap clears still present
  AC4 gate wiring

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

    src = _read("src/compiler/evaluator_primitives_memory.cpp")
    test = _read("tests/compiler/test_gc_heap_cells_clear_2486.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    idx = src.find('add("gc-heap"')
    end = src.find('add("gc-freeze"', idx)
    if idx >= 0 and end > idx:
        body = src[idx:end]
    elif idx >= 0:
        body = src[idx : idx + 5000]
    else:
        body = ""

    must("Issue #2486", "AC1", src)
    must("cells_.clear()", "AC1", body)
    must("cells_.shrink_to_fit()", "AC1", body)

    must("2486", "AC2", body)
    must("Audit", "AC2", body)

    must("string_heap_.clear()", "AC3", body)
    must("pairs_.clear()", "AC3", body)
    must("vector_heap_.clear()", "AC3", body)
    must("opaque_heap_.clear()", "AC3", body)
    must("error_values_.clear()", "AC3", body)

    must("2486 AC1", "AC4", test)
    must("check_gc_heap_cells_clear_2486", "gate", build)
    must("cmd_gc_heap_cells_clear_coverage", "gate", build)
    must("test_gc_heap_cells_clear_2486", "gate", cmake)
    must("2486 AC4", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: gc-heap cells clear #2486 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
