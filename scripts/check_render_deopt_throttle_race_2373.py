#!/usr/bin/env python3
"""Issue #2373: try_render_deopt_throttle CAS race fix coverage.

Contract:
  AC1 CAS loop (compare_exchange_weak) replaces load/store check-then-act
  AC2 N concurrent within window → exactly one true (unit test)
  AC3 Sequential outside window preserved (unit test)
  AC4 Test + this linter wired into pre-push gate

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

    h = _read("src/core/arena_auto_policy_stats.h")
    test = _read("tests/compiler/test_render_deopt_throttle_race_2373.cpp")
    cmake = _read("CMakeLists.txt")
    bp = _read("build.py")

    # AC1 CAS fix in header
    must("try_render_deopt_throttle", "AC1", h)
    must("compare_exchange_weak", "AC1", h)
    must("Issue #2373", "AC1", h)
    must("last_render_deopt_ns", "AC1", h)
    must("render_jit_deopt_applied_total", "AC1", h)
    must("render_jit_deopt_throttled_total", "AC1", h)
    # No bare check-then-act store of last_render_deopt_ns after load without CAS
    if "last_render_deopt_ns.store(now_ns" in h:
        fails.append("AC1: last_render_deopt_ns.store(now_ns still present (use CAS)")

    # AC2 concurrent unit test
    must("AC1: exactly one true", "AC2", test)
    must("try_render_deopt_throttle", "AC2", test)
    must("std::thread", "AC2", test)
    must("Issue #2373", "AC2", test)

    # AC3 sequential outside window
    must("AC2: first apply", "AC3", test)
    must("AC2: second apply after window", "AC3", test)

    # AC4 wire-up
    must("test_render_deopt_throttle_race_2373", "AC4", cmake)
    must("cmd_render_deopt_throttle_race_coverage", "AC4", bp)
    must("check_render_deopt_throttle_race_2373.py", "AC4", bp)
    must("AC3: CAS present", "AC4", test)
    must("AC4:", "AC4", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2373 render deopt throttle CAS race — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
