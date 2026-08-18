#!/usr/bin/env python3
"""Issue #2905: Moving densify sticky densify-off auto-clear + Agent visibility.

Contract:
  AC1 clean Moving densify / Phase-5 unified success clears sticky
  AC2 query exposes sticky flag + total (schema-2905 aliases)
  AC3 production hard arms sticky; Soft never arms
  AC4 re-register + clean Moving restores densify without manual clear
  AC5 source-cite + tests + build wire; no docs/design/*

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

    arena = _read("src/core/arena.ixx")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    evals = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")

    # AC1 — clean clear (per-arena + Phase-5)
    must("#2905", "AC1", arena)
    must("clear_moving_incomplete_remap_sticky_densify_off", "AC1", arena)
    # Issue #3123: auto-clear moved after RootRemap + stale so incomplete
    # windows cannot clear sticky. Healthy window (incl. zero-move clean)
    # replaces the early objects_moved>0 && !incomplete site.
    must("kStickyClearHealthyWindow", "AC1 clean", arena)
    must("!result.moving_incomplete_remap && result.pin_contract_held", "AC1 healthy", arena)
    must("clear_moving_incomplete_remap_sticky_densify_off", "AC1 Phase-5", mb)
    must("moving_unified_success", "AC1 Phase-5", mb)
    must("#2905", "AC1 Phase-5", mb)

    # AC2 — query surface
    must("sticky-densify-off", "AC2", obs)
    must("sticky-densify-off-total", "AC2", obs)
    must("moving-sticky-densify-off", "AC2", obs)
    must("moving-sticky-densify-off-total", "AC2", obs)
    must("schema-2905", "AC2", obs)
    must("moving-sticky-auto-clear-wired", "AC2", obs)
    must("moving-sticky-densify-off", "AC2 arena-live", evals)
    must("schema-2905", "AC2 arena-live", evals)

    # AC3 — hard arms / Soft never
    must("hard_pref > 0", "AC3", arena)
    must("Soft (hard_pref <= 0) does not arm sticky", "AC3", arena)
    must("g_moving_incomplete_remap_sticky_densify_off", "AC3", arena)

    # AC4 — recovery without manual clear
    must("ac2905_4_reregister_clean_restores_without_manual_clear", "AC4", test)

    # AC5 — tests + build + no design
    must("ac2905_1_clean_moving_clears_sticky", "AC5", test)
    must("ac2905_2_query_sticky_surface", "AC5", test)
    must("ac2905_3_hard_arms_soft_never", "AC5", test)
    must("ac2905_5_source_cite_phase5_no_design", "AC5", test)
    must("check_moving_sticky_densify_off_2905", "AC5", build)
    must("cmd_moving_sticky_densify_off_2905", "AC5", build)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2905-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "core" / "test_issue_2905.cpp").is_file():
        fails.append("tests/core/test_issue_2905.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2905 moving sticky densify-off auto-clear — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
