#!/usr/bin/env python3
"""Issue #2893: adaptive pure-anon remount budget + pressure signal
(refine #2850).

#2850 shipped a FIXED pure-anon sync remount budget (default 64, env
AURA_SYNC_REMOUNT_PURE_ANON_BUDGET, 0 = off) on reemit success. Under
sustained agent/EDSL denseness a static 64 leaves a measurable
native-hole window after every successful reemit, and Agents cannot
tune before the next mutation wave. #2893 makes the budget adaptive
under production and exposes a pressure signal:
  AC1 budget expands within a ceiling (256) when pure-anon skip /
      deopt-window pressure is high; skip counter rises only above
      the ceiling
  AC2 Soft / explicit budget=0 / low pressure -> fixed or zero path;
      no adaptive walk cost
  AC3 named (#2602) + captured (#2691/#2714) filters unchanged;
      opposite-sid invariants hold
  AC4 query surface additive (schema-2893 / issue-2893 / wired);
      schema-2850 keys preserved
  AC5 source-cite + linter; extend existing pure-anon / anonymous
      residual suite per #81967; no docs/design per #1655

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    br = _read("src/compiler/aura_jit_bridge.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    sh = _read("src/compiler/runtime_shared.h")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    test = _read("tests/compiler/test_anonymous_residual_stable_id_policy.cpp")
    build = _read("build.py")

    # AC1 — adaptive budget: pressure state + ceiling + walk-outcome feed.
    must("Issue #2893", "AC1", rt)
    must("g_pure_anon_pressure_bp", "AC1", rt)
    must("g_pure_anon_budget_current", "AC1", rt)
    must("kPureAnonBudgetCeiling", "AC1", rt)
    must("aura_pure_anon_note_walk_outcome", "AC1", rt)
    must("aura_pure_anon_note_walk_outcome(ok, skip)", "AC1", rt)
    must("aura_pure_anon_observe_deopt_window", "AC1", br)
    must("deopt_window_count", "AC1", br)

    # AC2 — Soft / budget=0 / low pressure -> fixed or zero path.
    must("production_defaults_active()", "AC2", rt)
    must("Soft / sandbox / tests → 0", "AC2", rt)
    must("should_throttle_reemit()", "AC2", br)
    must("aura_sync_remount_pure_anon_budget_base()", "AC2", br)

    # AC3 — named + captured filters unchanged (opposite-sid invariants).
    must("sid != 0", "AC3", rt)
    must("aura_closure_has_env_or_linear_captures", "AC3", rt)
    must("no double", "AC3", rt)

    # AC4 — query surface additive; schema-2850 preserved.
    must("live-closure-sync-remount-pure-anon-budget-current", "AC4", obs)
    must("live-closure-sync-remount-pure-anon-pressure-bp", "AC4", obs)
    must("live-closure-sync-remount-pure-anon-adaptive-wired", "AC4", obs)
    must("schema-2893", "AC4", obs)
    must("issue-2893", "AC4", obs)
    must("schema-2850", "AC4", obs)
    must("issue-2850", "AC4", obs)
    # C ABI declarations + weak stubs for light-link binaries.
    must("aura_sync_remount_pure_anon_budget_base", "AC4", sh)
    must("aura_pure_anon_note_walk_outcome", "AC4", sh)
    must("aura_pure_anon_observe_deopt_window", "AC4", sh)
    must("aura_sync_remount_pure_anon_budget_current", "AC4", sh)
    must("aura_pure_anon_pressure_bp", "AC4", sh)
    must("aura_sync_remount_pure_anon_budget_base", "AC4", stub)
    must("aura_pure_anon_note_walk_outcome", "AC4", stub)

    # AC5 — src/-aligned suite extended per #81967; linter wired; no docs.
    for fn in (
        "ac2893_1_adaptive_budget",
        "ac2893_2_soft_zero_cost",
        "ac2893_3_named_captured_unchanged",
        "ac2893_4_query_additive",
        "ac2893_5_source_and_linter",
    ):
        must(fn, "AC5", test)
    must("check_pure_anon_adaptive_budget_2893.py", "AC5", build)
    if _read("tests/issues/test_issue_2893.cpp"):
        fails.append("AC5: no tests/issues/test_issue_2893.cpp (per #81967)")
    if _read("docs/design/2893-pure-anon-adaptive-budget.md"):
        fails.append("AC5: no docs/design/2893-* (per #1655)")

    if fails:
        print("check_pure_anon_adaptive_budget_2893.py: FAIL")
        for f in fails:
            print("  - " + f)
        return 1
    print("check_pure_anon_adaptive_budget_2893.py: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
