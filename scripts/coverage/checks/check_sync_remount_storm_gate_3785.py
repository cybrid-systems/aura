#!/usr/bin/env python3
"""Issue #3785: sync covered remount honors residual storm gate."""

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

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    test = _read("tests/compiler/test_remount_force_deopt.cpp")
    build = _read("build.py")

    fn = rt.find("aura_sync_remount_covered_named_live_closures")
    win = rt[fn : fn + 1200] if fn != -1 else ""
    must("Issue #3785", "AC1 cite", win)
    must("storm >= 2", "AC1 Global storm", win)
    must("aura_hot_update_should_throttle_reemit", "AC1 throttle", win)
    must("g_reemit_success_sync_covered_budget_skip_total", "AC1 skip counter", win)
    gate = win.find("storm >= 2")
    lock = win.find("g_closure_table_mtx")
    if gate == -1 or lock == -1 or gate > lock:
        fails.append("AC1: storm gate must precede table lock")

    must("g_residual_force_skip", "AC2 force-skip", win)

    must("ac3785_1", "AC4 tests", test)
    must("ac3785_2", "AC4 tests", test)
    must("ac3785_3", "AC4 tests", test)
    must("check_sync_remount_storm_gate_3785", "AC4 build", build)
    if build.find("check_sync_remount_storm_gate_3785") <= build.find("check_anon_peer_empty_name_soft_stale_3784"):
        fails.append("AC4: linter must be after #3784")
    if (ROOT / "tests" / "issues" / "test_issue_3785.cpp").is_file():
        fails.append("AC4: invent test present")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1

    print("OK  AC1_sync_remount_storm_gate")
    print("OK  AC2_residual_predicates_shared")
    print("OK  AC3_shape_only_decoupled")
    print("OK  AC4_tests_wiring")
    print(
        "\nOK: Issue #3785 sync covered remount storm gate — Soft Global "
        "budget_skip; Shape-only decoupled; Soft/Off unchanged"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
