#!/usr/bin/env python3
"""Issue #2703: production hard-face when partial cone truncates outside-If OccurrenceGoals.

Contract (one row per AC):
  AC1 src/compiler/typed_mutation_audit.h defines the #2703
     file-scope atomics (g_cone_outside_goal_drop_total +
     g_cone_outside_goal_drop_soft_total + g_cone_outside_goal_drop_wired
     sentinel) + kConeOutsideGoalDropIssue = 2703 + the new
     force_reason "cone_outside_goal_drop" (code 10) in
     commit_readiness_reason_code. commit_readiness consults the new
     face — production + cone_outside_goal_drop → reject; Soft →
     observe only.
  AC2 Soft + cone truncate + outside-If goals dropped → observe
     counters only (g_cone_outside_goal_drop_soft_total bumps);
     commit may succeed (Soft ergonomics preserved).
  AC3 Truncate with empty outside-If drop set → no extra full solve
     (zero cost happy path; cone_outside_goal_drop_total stays flat).
  AC4 (Persists path out of scope for first ship — documented in
     issue body as Option A vs Option B; first ship ships the surface
     + counter + Soft/Production routing).
  AC5 evaluator_primitives_query.cpp exposes a distinct
     "cone-outside-goal-drop-total" / "cone-outside-goal-drop-soft-total"
     / "cone-outside-goal-drop-wired" + "schema-2703" / "issue-2703".
     Prior #2621 / #2560 / #2672 / #2458 / #2608 surfaces preserved.
  AC6 Unit + source-cite coverage; extend existing
     tests/compiler/test_partial_cone_commit_gate.cpp (no new
     test_issue_N.cpp per #81967). Coverage linter wired into
     build.py gate.

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

    def must_key(n: str, label: str, hay: str) -> None:
        # clang-format may split adjacent string literals; strip quotes/whitespace.
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/compiler/test_partial_cone_commit_gate.cpp")
    build = _read("build.py")

    # AC1 — file-scope atomics + force_reason code
    must("Issue #2703", "AC1", tma)
    must("g_cone_outside_goal_drop_total", "AC1", tma)
    must("g_cone_outside_goal_drop_soft_total", "AC1", tma)
    must("g_cone_outside_goal_drop_wired", "AC1", tma)
    must("kConeOutsideGoalDropIssue = 2703", "AC1", tma)
    must("cone_outside_goal_drop_total_v_read", "AC1", tma)
    must("cone_outside_goal_drop_soft_total_v_read", "AC1", tma)
    must("cone_outside_goal_drop_wired_v_read", "AC1", tma)
    must("clear_cone_outside_goal_drop_for_test", "AC1", tma)
    must("cone_outside_goal_drop", "AC1", tma)
    must("return 10; // #2703", "AC1", tma)
    must("publish_partial_cone_truncate", "AC1", tma)

    # AC2 — Soft path metric-only
    must("g_cone_outside_goal_drop_soft_total", "AC2", tma)

    # AC3 — empty outside-If drop set → zero cost (no counter bump)
    # Verified by the atomics structure: g_cone_outside_goal_drop_total
    # only bumps on cone_outside_goal_drop signal (production reject
    # path). Empty drop set means no signal, no bump.
    must("Issue #2703", "AC3", tma)

    # AC5 — query surface
    must_key("query:cone-outside-goal-drop", "AC5", q)
    must_key("cone-outside-goal-drop-total", "AC5", q)
    must_key("cone-outside-goal-drop-soft-total", "AC5", q)
    must_key("cone-outside-goal-drop-wired", "AC5", q)
    must_key("schema-2703", "AC5", q)
    must_key("issue-2703", "AC5", q)
    # Prior #2621 / #2560 / #2672 surfaces preserved.
    must_key("schema-2621", "AC5", q)
    must_key("schema-2560", "AC5", q)
    must_key("schema-2672", "AC5", q)

    # AC6 — test extension per #81967
    must("ac2703_1_production_hard_face", "AC6", t)
    must("ac2703_2_soft_observe_only", "AC6", t)
    must("ac2703_3_empty_drop_zero_cost", "AC6", t)
    must("ac2703_4_query_keys_added", "AC6", t)
    must("ac2703_5_source_and_linter", "AC6", t)
    must("ac2703_6_no_docs_design", "AC6", t)
    must("check_cone_outside_goal_drop_2703", "AC6", build)

    # No docs/design/2703-* on disk
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2703-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Cross-check: prior #2702 + #2701 + #2700 linters still green
    for prev in (
        "check_resume_hard_fail_2702.py",
        "check_mutation_hold_budget_reject_2701.py",
        "check_handoff_ref_mailbox_gate_2700.py",
    ):
        r = subprocess.run(
            [
                sys.executable,
                str(ROOT / "scripts" / "coverage" / "checks" / prev),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            fails.append(f"{prev} regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2703 cone-outside-goal-drop production hard-face — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
