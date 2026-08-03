#!/usr/bin/env python3
"""Issue #2373 / #2626: try_render_deopt_throttle CAS race fix coverage.

After #2626 (TUI/render present surface removed), the dedicated unit test
`test_render_deopt_throttle_race_2373` was deleted with the TUI stack.
This gate keeps the production CAS contract:

  AC1 CAS loop (compare_exchange_weak) in arena_auto_policy_stats.h
  AC2 no bare last_render_deopt_ns.store(now_ns check-then-act
  AC3 this linter + build.py gate remain wired

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
    bp = _read("build.py")
    self_src = _read("scripts/check_render_deopt_throttle_race_2373.py")

    must("try_render_deopt_throttle", "AC1", h)
    must("compare_exchange_weak", "AC1", h)
    must("Issue #2373", "AC1", h)
    must("last_render_deopt_ns", "AC1", h)
    must("render_jit_deopt_applied_total", "AC1", h)
    must("render_jit_deopt_throttled_total", "AC1", h)
    if "last_render_deopt_ns.store(now_ns" in h:
        fails.append("AC2: last_render_deopt_ns.store(now_ns still present (use CAS)")

    must("cmd_render_deopt_throttle_race_coverage", "AC3", bp)
    must("check_render_deopt_throttle_race_2373.py", "AC3", bp)
    must("#2626", "AC3", self_src)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2373 render deopt CAS retained after #2626 TUI removal")
    return 0


if __name__ == "__main__":
    sys.exit(main())
