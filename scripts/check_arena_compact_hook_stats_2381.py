#!/usr/bin/env python3
"""Issue #2381: concurrent compact_hook shape_inval counter is race-free.

Contract:
  AC1 Concurrent hook invoke path uses atomic RMW (TSAN-clean counter)
  AC2 Counter matches # invocations (fetch_add, no lost updates)
  AC3 GUARDED_BY audit on serial stats_ fields OR atomic concurrent-hot
  AC4 Tests + CMake + gate registration; compact() still invokes hook

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

    def must_absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden residual {n!r}")

    arena = _read("src/core/arena.ixx")
    test = _read("tests/core/test_arena_compact_hook_stats_2381.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1 concurrent-safe path
    must("shape_inval_on_compact_", "AC1", arena)
    must("std::atomic<std::size_t> shape_inval_on_compact_", "AC1", arena)
    must("shape_inval_on_compact_.fetch_add", "AC1", arena)
    must("invoke_on_compact_hook_for_test", "AC1", arena)
    must("ac1_ac2_concurrent_hook_counter", "AC1", test)
    must("kThreads = 4", "AC1", test)

    # AC2 exact count
    must("matches invocations exactly", "AC2", test)
    must("shape_inval_on_compact_relaxed", "AC2", arena)
    must_absent("stats_.shape_inval_on_compact++", "AC2", arena)

    # AC3 audit consistency
    must("GUARDED_BY(per-arena compact serial)", "AC3", arena)
    must("Issue #2381", "AC3", arena)
    must("root_remap_stable_ref_total_", "AC3", arena)
    must("root_remap_stable_ref_total_.fetch_add", "AC3", arena)
    must("ac3_source_audit", "AC3", test)

    # AC4 wiring
    must("invoke_compact_hook_()", "AC4", arena)
    must("test_arena_compact_hook_stats_2381", "AC4", cmake)
    must("check_arena_compact_hook_stats_2381", "AC4", build)
    must("cmd_arena_compact_hook_stats_coverage", "AC4", build)
    must("ac4_wiring", "AC4", test)
    must("Issue #2381", "AC4", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2381 concurrent compact_hook shape_inval — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
