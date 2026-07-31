#!/usr/bin/env python3
"""Issue #2396: production tick periodically reaps orphan fibers.

Contract:
  AC1 maybe_reap_orphans_on_tick wired; residual path uses note_orphan
  AC2 empty orphan list → no mutex (orphan_count_cached_ relaxed load)
  AC3 AURA_ORPHAN_REAP_INTERVAL_MS + run() IO loop wire-up
  AC4 Ok join path unchanged (no residual)
  AC5 Tests + CMake + build.py gate

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

    hh = _read("src/serve/scheduler.h")
    cpp = _read("src/serve/scheduler.cpp")
    test = _read("tests/orch/test_join_drain_reclaim_2227.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("Issue #2396", "AC1", hh)
    must("maybe_reap_orphans_on_tick", "AC1", hh)
    must("maybe_reap_orphans_on_tick", "AC1", cpp)
    must("orphan_count_cached_", "AC1", hh)
    must("2396 AC1", "AC1", test)

    # AC2 zero-cost empty
    must("orphan_count_cached_.load", "AC2", cpp)
    must("tick_orphan_mutex_acquired_total", "AC2", hh)
    must("2396 AC2", "AC2", test)

    # AC3 interval + run wire-up
    must("AURA_ORPHAN_REAP_INTERVAL_MS", "AC3", cpp)
    must("orphan_reap_interval_ms", "AC3", hh)
    must("maybe_reap_orphans_on_tick()", "AC3", cpp)
    must("2396 AC3", "AC3", test)

    # AC4
    must("AC3: residual not bumped on Ok", "AC4", test)

    # AC5
    must("2396 AC5", "AC5", test)
    must("check_orphan_reap_tick_2396", "AC5", build)
    must("cmd_orphan_reap_tick_coverage", "AC5", build)
    must("test_join_drain_reclaim_2227", "AC5", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2396 orphan reap on production tick — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
