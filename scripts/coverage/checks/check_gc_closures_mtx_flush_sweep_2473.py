#!/usr/bin/env python3
"""Issue #2473: flush_gc_roots / compact_sweep take closures_mtx_.

Contract:
  AC1 concurrent stress test present
  AC2 shared_lock flush + unique_lock sweep
  AC3 metrics counters present + bumped
  AC4 lock-order Closures / #1664 cite
  AC5 gate wiring

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

    gc = _read("src/compiler/evaluator_gc.cpp")
    met = _read("src/compiler/observability_metrics.h")
    ixx = _read("src/compiler/evaluator.ixx")
    test = _read("tests/compiler/test_gc_closures_mtx_flush_sweep_2473.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    fidx = gc.find("void Evaluator::flush_gc_roots")
    flush = gc[fidx : fidx + 2800] if fidx >= 0 else ""
    # Prefer the out-of-line definition (not forward decls / comments).
    # Closures unique_lock is ~4.6k into the body (after defer checks).
    sidx = gc.find("Evaluator::CompactSweepResult Evaluator::compact_sweep")
    if sidx < 0:
        sidx = gc.find("Evaluator::compact_sweep(void*")
    if sidx < 0:
        sidx = gc.rfind("compact_sweep(void*")
    sweep = gc[sidx : sidx + 7000] if sidx >= 0 else ""

    must("Issue #2473", "AC1", gc)
    must("2473 AC1", "AC1", test)
    must("compact_sweep", "AC1", test)
    must("register_active_closure", "AC1", test)

    must("shared_lock", "AC2", flush)
    must("closures_mtx_", "AC2", flush)
    must("unique_lock", "AC2", sweep)
    must("closures_mtx_", "AC2", sweep)
    must("Issue #2473", "AC2", flush)
    must("Issue #2473", "AC2", sweep)

    must("gc_flush_closures_locked_total", "AC3", met)
    must("gc_sweep_closures_locked_total", "AC3", met)
    must("gc_flush_closures_locked_total", "AC3", flush)
    must("gc_sweep_closures_locked_total", "AC3", sweep)
    must("2473 AC3", "AC3", test)

    must("Level::Closures", "AC4", flush)
    must("Level::Closures", "AC4", sweep)
    must("#1664", "AC4", gc)
    must("2473 AC4", "AC4", test)
    must("#2473", "AC4", ixx)

    must("check_gc_closures_mtx_flush_sweep_2473", "gate", build)
    must("cmd_gc_closures_mtx_flush_sweep_coverage", "gate", build)
    must("test_gc_closures_mtx_flush_sweep_2473", "gate", cmake)
    must("2473 AC5", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: GC flush/sweep closures_mtx_ #2473 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
