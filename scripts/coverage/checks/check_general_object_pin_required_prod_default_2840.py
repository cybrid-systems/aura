#!/usr/bin/env python3
"""Issue #2840: LifetimePin general-object required mode densify fail-closed.

Residual of #2597/#2665: production defaults lock required, but callers
ignored wire failures and densify was not gated. #2840:
  AC1 sticky breach on required wire fail + densify fail-closes
  AC2 create paths use wire_*_or_required_fail (not void cast)
  AC3 Soft / pref<=0 observe-only (no breach, helper returns true)
  AC4 production security defaults still lock pref=1 (#2597 lineage)
  AC5 query schema-2840 + densify-fail counter; #2665 keys preserved
  AC6 test extension + linter; no docs/design/; no invent file

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


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

    lp = _read("src/core/lifetime_pin.hh")
    arena = _read("src/core/arena.ixx")
    sec = _read("src/compiler/security_defaults.hh")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    ev = _read("src/compiler/evaluator_primitives_eval.cpp")
    test = _read("tests/core/test_general_object_pin_coverage_gate.cpp")
    build = _read("build.py")

    # AC1 — breach + densify gate
    must("Issue #2840", "AC1", lp)
    must("g_general_object_pin_required_breach", "AC1", lp)
    must("general_object_pin_required_active", "AC1", lp)
    must("g_general_object_pin_required_breach.store(1", "AC1", lp)
    must("general_object_pin_required_breach_active", "AC1", arena)
    must("g_general_object_pin_required_breach_densify_fail_total", "AC1", arena)
    must("clear_general_object_pin_required_breach", "AC1", arena)

    # AC2 — callers fail-closed
    must("wire_general_object_create_pair_or_required_fail", "AC2", lp)
    must("wire_general_object_create_pair_or_required_fail", "AC2", mut)
    must("wire_general_object_create_pair_or_required_fail", "AC2", ev)
    # No void-cast of the bare wire in mutate (use required-fail helper).
    if "(void)aura::core::lifetime::wire_general_object_create_pair(" in mut:
        fails.append("AC2: mutate still void-casts bare wire_general_object_create_pair")

    # AC3 — Soft observe-only
    must("Soft: observe-only", "AC3", lp)
    must("return true", "AC3", lp)

    # AC4 — production defaults lineage
    must("#2840", "AC4", sec)
    must("g_general_object_pin_required_pref.store(1, std::memory_order_release)", "AC4", sec)
    must("AURA_GENERAL_OBJECT_PIN", "AC4", sec)

    # AC5 — query surface
    must("schema-2840", "AC5", obs)
    must("general-object-pin-required-breach", "AC5", obs)
    must("general-object-pin-required-breach-densify-fail-total", "AC5", obs)
    must("schema-2665", "AC5", obs)

    # AC6 — test + linter + no invent/design
    must("ac2840_1_breach_and_densify_gate", "AC6", test)
    must("ac2840_2_callers_fail_closed", "AC6", test)
    must("check_general_object_pin_required_prod_default_2840", "AC6", build)
    if (ROOT / "tests" / "core" / "test_issue_2840.cpp").is_file():
        fails.append("AC6: test_issue_2840.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2840-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Cross-check #2665 / #2597 still green
    for linter in (
        "check_2665_coverage.py",
        "check_general_object_pin_auto_wire_2597.py",
    ):
        r = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "coverage" / "checks" / linter)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            fails.append(f"{linter} regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2840 GeneralObjectPin required densify fail-closed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
