#!/usr/bin/env python3
"""Issue #3421: production apply_closure hard-refuses densify-stale closures.

#2569 restamp is not a remap. Under production + last Moving
objects_moved>0 + (remap key OR LCP deny), apply_closure returns
nullopt and must not restamp-and-eval_flat. Soft / no-move keep
#2569 recover.

Contract:
  AC1 helper after needs_safe_fallback / must_deopt / race recover
  AC2 Soft / objects_moved==0 skip remap (production load + moved load)
  AC3 no invoke_closure_bridge_checked on densify hard-refuse
  AC4 reuse closure_stale_returns; no g_3421_* / no new query key
  AC5 tests in test_setcode_rebind_survive; linter after #3420;
      no docs/design/3421-*; no test_issue_3421.cpp

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

    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    survive = _read("tests/compiler/test_setcode_rebind_survive.cpp")
    build = _read("build.py")

    must("kApplyClosureDensifyHardRefuseIssue = 3421", "AC1 stamp", flat)
    must("production_apply_closure_densify_hard_refuse", "AC1 helper", flat)
    must("Issue #3421", "AC1 eval_flat cite", flat)
    must("Issue #3421", "AC1 evaluator.ixx cite", ev)

    helper = flat.find("static bool production_apply_closure_densify_hard_refuse")
    helper_end = flat.find("static void note_apply_closure_densify_hard_refuse", helper)
    helper_win = flat[helper:helper_end] if helper >= 0 and helper_end > helper else flat[helper : helper + 800]
    must("production_defaults_active()", "AC2 production load", helper_win)
    must("g_last_objects_moved", "AC2 last-window moved", helper_win)
    must("last_lifetime_consistency_would_allow()", "AC1 LCP", helper_win)
    must("resolve_object_remap", "AC1 remap key", helper_win)
    if "LifetimePin::pin" in helper_win or ".pin(" in helper_win:
        fails.append("AC2: extra pin walk on densify-refuse helper")
    if "invoke_closure_bridge_checked" in helper_win:
        fails.append("AC3: helper must not soft-migrate onto native bridge")

    calls = flat.count("production_apply_closure_densify_hard_refuse(arena_, cl_copy)")
    if calls < 3:
        fails.append(f"AC1: expected 3 apply-site helper calls, found {calls}")
    # Recover sites: helper call must appear in the same window as the restamp cite.
    for label, needle, before in (
        ("must_deopt", "if (cl_copy.must_deopt_before_next_call)", False),
        ("pre-materialize #2569", "apply_closure_soft_recover_2569", True),
        ("race-window #2569", "apply_closure_race_soft_recover_2569", True),
    ):
        pos = flat.find(needle)
        if pos < 0:
            fails.append(f"AC1: missing {label} site {needle!r}")
            continue
        win = flat[pos : pos + 1200] if not before else flat[max(0, pos - 2800) : pos + 80]
        if "production_apply_closure_densify_hard_refuse(arena_, cl_copy)" not in win:
            fails.append(f"AC1: {label} must consult densify hard-refuse before recover")

    must("closure_stale_returns.fetch_add", "AC4 reuse stale_returns", flat)
    if "g_3421_" in flat or "g_3421_" in ev:
        fails.append("AC4: invented g_3421_* counter")
    if "schema-3421" in flat or "schema-3421" in ev:
        fails.append("AC4: new schema-3421 query key")
    if "class DensifyClosurePinRegistry" in flat or "g_moving_pin_registry_3421" in flat:
        fails.append("AC4: second pin registry introduced")

    must("ac5_3421_production_hard_refuse", "AC5 test", survive)
    must("ProdDensifyWindowGuard", "AC5 inject guard", survive)
    must("check_apply_closure_densify_hard_refuse_3421", "AC5 build.py", build)
    prev = build.find("check_factory_refuse_uncovered_3420")
    ours = build.find("check_apply_closure_densify_hard_refuse_3421")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: #3421 linter must run after #3420")
    if (ROOT / "tests" / "issues" / "test_issue_3421.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3421.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3421.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3421.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3421-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3421 apply_closure_densify_hard_refuse:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3421 apply_closure_densify_hard_refuse: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
