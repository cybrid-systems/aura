#!/usr/bin/env python3
"""Issue #3060: production residual budget_skip force-leaves pending
pure-anon (bounded). Reuses #3024 MustDeopt helper. Soft / named /
steal unchanged. No new query keys.

  AC1  production + repeated budget_skip (or pending ≥ thresh) → MustDeopt
  AC2  Soft / !production / budget=0: skip counter only
  AC3  named sync walk still excludes sid==0; steal-complete does not drain
  AC4  remount fail uses shared MustDeopt; soak leaves native
  AC5  tests + build.py; no invent / docs/design; not a second table

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

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    steal = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/compiler/test_anonymous_residual_stable_id_policy.cpp")
    build = _read("build.py")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")

    # AC1
    must("Issue #3060", "AC1", rt)
    must("pure_anon_pressure_force_leave_oldest", "AC1 helper", rt)
    must("kPureAnonBudgetSkipStreakForce", "AC1 streak", rt)
    must("kPureAnonPressurePendingThresh", "AC1 thresh", rt)
    must("pure_anon_bg_overflow_force_leave_native", "AC1 reuse 3024", rt)
    must("g_residual_budget_skip_streak", "AC1 streak atom", rt)
    must("ac3060_1_prod_skip_streak_must_deopt", "AC1 test", test)

    skip = rt.find('extern "C" void aura_residual_live_closure_remount_tick')
    if skip < 0:
        fails.append("AC1: residual tick missing")
    else:
        body = rt[skip : skip + 2200]
        if "pure_anon_pressure_force_leave_oldest" not in body:
            fails.append("AC1: tick budget_skip does not force-leave")
        if "production_defaults_active()" not in body:
            fails.append("AC1: tick missing production gate")

    # AC2
    must("g_residual_budget_skip_streak.store(0", "AC2 budget=0 reset", rt)
    must("ac3060_2_soft_skip_no_force", "AC2 test", test)
    must("Soft / Off: skip counter only", "AC2 cite", rt)

    # AC3 named + steal
    must("Issue #3060: pure-anon", "AC3 named", rt)
    must("does not promote sid==0", "AC3 covered-named", rt)
    must("ac3060_3_named_and_steal_unchanged", "AC3 test", test)
    pos = steal.find("aura_evaluator_on_steal_complete")
    if pos < 0:
        fails.append("AC3: steal-complete missing")
    else:
        win = steal[pos : pos + 8000]
        if "aura_pure_anon_bg_remount_drain" in win:
            fails.append("AC3: steal drains pure-anon")
        if "aura_residual_live_closure_remount_tick" in win:
            fails.append("AC3: steal residual-ticks")
        if "pure_anon_pressure_force_leave_oldest" in win:
            fails.append("AC3: steal pressure-force")

    # AC4 shared fail + soak
    must("Issue #3060: remount fail uses the shared MustDeopt path", "AC4 drain fail", rt)
    must("Issue #3060: residual / drain fail-closed", "AC4 remount fail", rt)
    must("ac3060_4_soak_bounded_leave", "AC4 soak", test)

    # AC5 — no new query keys
    if "schema-3060" in obs:
        fails.append("AC5: new schema-3060 query key (forbidden)")
    must("check_pure_anon_pressure_force_leave_3060", "AC5 build", build)
    must("cmd_pure_anon_pressure_force_leave_3060", "AC5 cmd", build)
    must("ac3060_5_source_and_linter", "AC5 test", test)
    cite = rt.find("Issue #3060")
    if cite >= 0 and "AgentRegistry" in rt[cite : cite + 2500]:
        fails.append("AC5: must not introduce AgentRegistry")
    if (ROOT / "tests" / "compiler" / "test_issue_3060.cpp").is_file():
        fails.append("AC5: test_issue_3060.cpp present (forbidden per #81967)")
    if _read("docs/design/3060-pure-anon-pressure-force.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print(f"Issue #3060 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3060 pure-anon pressure force-leave — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
