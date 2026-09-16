#!/usr/bin/env python3
"""Issue #3812: Soft Global x critical bypass success covered remount gate."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    # Whitespace-normalized: pins must survive clang-format reflows of the
    # scanned sources (the gate runs format + these checks in one pass).
    p = ROOT / rel
    if not p.is_file():
        return ""
    return " ".join(p.read_text(encoding="utf-8", errors="replace").split())


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    reg = _read("src/compiler/hot_update_registry.cpp")
    hh = _read("src/compiler/hot_update_registry.hh")
    rt = _read("src/compiler/aura_jit_runtime.cpp")
    br = _read("src/compiler/aura_jit_bridge.cpp")
    test = _read("tests/compiler/test_remount_force_deopt.cpp")
    build = _read("build.py")

    must("Issue #3812", "AC1 registry", reg)
    must("soft_global", "AC1 storm conjunct", reg)
    must("aura_note_reemit_success_sync_covered_budget_skip", "AC1 default deny", reg)
    must("allow_critical_bypass_sync_covered_remount", "AC1 policy", reg)
    call = reg.find("aura_sync_remount_covered_named_live_closures(cov, cap)")
    gate = reg.find("soft_global")
    if call == -1 or gate == -1 or gate > call:
        fails.append("AC1: soft_global gate must precede remount call")

    fn = rt.find("aura_sync_remount_covered_named_live_closures")
    helper = rt.find("aura_critical_bypass_covered_remount_allowed")
    win = rt[fn : fn + 1600] if fn != -1 else ""
    hwin = rt[helper : helper + 800] if helper != -1 else ""
    must("Issue #3812", "AC2 cite", win)
    must("storm >= 2", "AC2 Global gate", win)
    must("aura_critical_bypass_covered_remount_allowed", "AC2 allow helper", win)
    must("aura_hot_update_hard_storm_active", "AC2 hard ceiling", hwin)
    must("relower_success_define_active", "AC2 precise coverage", hwin)
    must("never full named FIFO", "AC2 no full FIFO", win + hwin)
    must("allow_critical_bypass_sync_covered_remount", "AC2 hh policy", hh)

    must("Shape-only", "AC3 bridge", br)
    must("storm==1", "AC3 runtime Shape cite", rt)

    must("Issue #3812", "AC4 bridge", br)
    must("clear_critical_bypass_remount_armed", "AC4 one-shot clear", reg)
    must("ac3812_1", "AC4 tests", test)
    must("ac3812_2", "AC4 tests", test)
    must("ac3812_3", "AC4 tests", test)
    must("ac3812_4", "AC4 tests", test)
    must("check_soft_global_critical_remount_3812", "AC4 build", build)
    if (ROOT / "tests" / "issues" / "test_issue_3812.cpp").is_file():
        fails.append("AC4: invent test present")
    if (ROOT / "tests" / "compiler" / "test_issue_3812.cpp").is_file():
        fails.append("AC4: invent test_issue_3812.cpp present")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1

    print("OK  AC1_registry_soft_global_conjunct")
    print("OK  AC2_hard_ceiling_critical_default_deny")
    print("OK  AC3_shape_only_passthrough")
    print("OK  AC4_soak_wiring")
    print(
        "\nOK: Issue #3812 Soft Global x critical bypass covered remount "
        "gate -- default deny; Shape-only unchanged; hard ceiling zero remount"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
