#!/usr/bin/env python3
"""Issue #3783: alloc-path Moving auto-arm publishes densify health.

#3739 published on the non-soft_gated success branch only. Incomplete/hard
paths armed sticky then Soft-fell-back while Agent health could stay
would_allow_mutate=true after objects already moved. Phase-5 also hardcoded
moving_blocked_precondition=false into compute_moving_unified_success
(vacuous-green on blocked windows).

#3783: publish densify health on every production auto-arm Moving attempt
(before Soft fallback); force pin false when blocked; pass real blocked
aggregate into Phase-5 unified success. Soft/Off never take the auto-arm
Moving arm (zero-cost).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    arena = _read("src/core/arena.ixx")
    mut = _read("src/compiler/evaluator_mutation_boundary.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")

    arm = arena.find("should_production_auto_arm_moving(frag_before)")
    win = arena[arm : arm + 5500] if arm != -1 else ""
    must("Issue #3783", "AC1 cite", win)
    must("publish_last_moving_densify_window", "AC1 publish", win)
    must("pin_for_publish", "AC1 pin_for_publish", win)
    must("!r.moving_blocked_precondition", "AC1 blocked forces pin false", win)
    pub = win.find("publish_last_moving_densify_window")
    soft = win.find("live_compact(/*force=*/false)")
    if pub == -1 or soft == -1 or pub > soft:
        fails.append("AC1: publish must precede Soft fallback live_compact(/*force=*/false)")

    must("moving_blocked_precondition_any", "AC2 AdaptiveCompactResult field", arena)
    must("Issue #3783", "AC2 mut cite", mut)
    must("densify_moving_blocked", "AC2 densify_moving_blocked", mut)
    must("densify_moving_blocked", "AC2 unified success arg", mut)
    must_not(
        "/*moving_blocked_precondition=*/false",
        "AC2 no hardcoded false",
        mut,
    )

    # Soft/Off: production auto-arm gate retained
    must("should_production_auto_arm_moving", "AC3 Soft gate", arena)

    must("ac3783_1", "AC4 ac3783_1", test)
    must("ac3783_2", "AC4 ac3783_2", test)
    must("ac3783_3", "AC4 ac3783_3", test)
    must("check_auto_arm_densify_health_publish_3783", "AC4 build wire", build)
    if build.find("check_auto_arm_densify_health_publish_3783") <= build.find(
        "check_densify_entry_lcp_skip_compact_3782"
    ):
        fails.append("AC4: linter must be wired in build.py AFTER #3782 linter")
    must_not("tests/issues/test_issue_3783.cpp", "AC4 no invent", test)
    if (ROOT / "tests" / "issues" / "test_issue_3783.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3783.cpp present (forbidden)")
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3783-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1

    print("OK  AC1_auto_arm_publish_before_soft_fallback")
    print("OK  AC2_phase5_real_blocked_precondition")
    print("OK  AC3_soft_off_zero_cost")
    print("OK  AC4_tests_wiring_no_invent")
    print(
        "\nOK: Issue #3783 auto-arm densify health publish — publish before Soft "
        "fallback; Phase-5 real blocked; Soft zero-cost"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
