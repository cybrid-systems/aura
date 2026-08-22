#!/usr/bin/env python3
"""Issue #3248: residual-force auto-heal ages on outermost fail exits.

#3096 auto-heal is implemented; observe was success-only so
failure-dominated mutate never reached kAutoHealExits=256.
Option A: call observe_residual_force_stale on any outermost exit.
Remount/drain stay success-only. Playbook stays observe-only.

Contract (one row per AC):
  AC1  outermost fail exits age; 256 fail → auto-heal once per mask
  AC2  Soft / Off / storm: zero extra reemit; auto-heal still suppressed
  AC3  remount/drain stay success-only; playbook observe-only
  AC4  reuse residual_force_auto_heal_total / stale_observe (no new keys)
  AC5  extend existing suites; no invent; no docs/design

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

    bnd = _read("src/compiler/evaluator_mutation_boundary.cpp")
    cpp = _read("src/compiler/hot_update_registry.cpp")
    hh = _read("src/compiler/hot_update_registry.hh")
    rec = _read("tests/compiler/test_reload_recovery_query.cpp")
    build = _read("build.py")

    obs = bnd.find("aura_hot_update_observe_residual_force_stale")
    if obs < 0:
        fails.append("AC1: observe hook missing")
    else:
        win = bnd[max(0, obs - 350) : obs + 80]
        must("Issue #3248", "AC1 cite", win)
        if "if (outermost)" not in win:
            fails.append("AC1: observe not gated by if (outermost)")
        # The observe if-line itself must not require success.
        line_start = bnd.rfind("\n", 0, obs)
        if_line = bnd[max(0, line_start - 80) : obs]
        if "outermost && success" in if_line:
            fails.append("AC1: observe still success-only")
        must("success or fail", "AC1 fail-exit comment", win)

    remount = bnd.find("aura_residual_live_closure_remount_tick(b)")
    if remount < 0:
        fails.append("AC3: remount tick missing")
    else:
        rwin = bnd[max(0, remount - 400) : remount]
        if "if (outermost && success)" not in rwin:
            fails.append("AC3: remount no longer success-only")
        if "aura_pure_anon_bg_remount_drain" not in bnd[remount : remount + 400]:
            fails.append("AC3: drain moved off success path")

    must("Issue #3248", "AC1 observe impl", cpp)
    must("success or fail", "AC1 header/cpp contract", hh + cpp)
    must("kAutoHealExits", "AC4 auto-heal gate reused", cpp)
    must("residual_force_auto_heal_total", "AC4 existing counter", cpp)
    must("maybe_coverage_verify_min_dirty(ReemitReason::ResidualForceHeal)", "AC4 reuse heal", cpp)
    must("aura_production_defaults_active_probe() == 0", "AC2 Soft skip", cpp)

    must("ac3248_1_fail", "AC1 fail-exit test", rec)
    must("ac3248_2_soft", "AC2 Soft test", rec)
    must("does not double-heal", "AC1 cap", rec)
    must("Playbook stays observe-only", "AC3 playbook", bnd)
    must("check_residual_force_fail_exit_age_3248", "AC5 build.py", build)
    must("cmd_residual_force_fail_exit_age_3248_coverage", "AC5 cmd", build)
    if _read("tests/compiler/test_issue_3248.cpp") or _read("tests/issues/test_issue_3248.cpp"):
        fails.append("AC5: test_issue_3248.cpp present (forbidden #81967)")
    if _read("docs/design/3248-residual-force-fail-exit.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3248 residual_force_fail_exit_age:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3248 residual_force_fail_exit_age: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
