#!/usr/bin/env python3
"""Issue #2430: snapshot_capability_effect_stats double-check (#1840 pattern).

Contract:
  AC1 16-retry loop + double-check hot counters
  AC2 concurrent gate test
  AC3 explicit memory_order_acquire loads
  AC4 field names preserved; gate wired

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

    hh = _read("src/core/capability_model.hh")
    test = _read("tests/core/test_capability_effect_stats_snapshot_2430.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2430", "AC1", hh)
    must("snapshot_capability_effect_stats", "AC1", hh)
    must("for (int attempt = 0; attempt < 16; ++attempt)", "AC1", hh)
    must("best-effort after retries", "AC1", hh)
    must("2430 AC1", "AC1", test)

    must("2430 AC2", "AC2", test)
    must("concurrent check + snapshot", "AC2", test)

    must("std::memory_order_acquire", "AC3", hh)
    # Double-check uses acquire on hot counters
    must(
        "m.capability_effect_enforced_total.load(std::memory_order_acquire) == s.enforced",
        "AC3",
        hh,
    )
    must(
        "m.capability_effect_denied_total.load(std::memory_order_acquire) == s.denied",
        "AC3",
        hh,
    )
    must("2430 AC3", "AC3", test)

    must("s.enforced", "AC4", hh)
    must("s.denied", "AC4", hh)
    must("2430 AC4", "AC4", test)
    must("check_capability_effect_stats_snapshot_2430", "gate", build)
    must("cmd_capability_effect_stats_snapshot_coverage", "gate", build)
    must("test_capability_effect_stats_snapshot_2430", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: capability effect stats snapshot #2430 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
