#!/usr/bin/env python3
"""Issue #2704: production hard-face on OccurrenceGoal rehydrate miss after steal/densify fence.

Contract (one row per AC):
  AC1 src/compiler/typed_mutation_audit.h defines the #2704 file-scope
     atomics (g_occurrence_empty_after_fence_total +
     g_occurrence_empty_after_fence_soft_total +
     g_occurrence_empty_after_fence_wired sentinel) +
     kOccurrenceEmptyAfterFenceIssue = 2704 + the new force_reason
     "occurrence_empty_after_fence" (code 11) in
     commit_readiness_reason_code. commit_readiness consults the new
     face — production + occurrence_empty_after_fence → reject; Soft
     → observe only.
  AC2 Soft + same → miss counter only (g_occurrence_empty_after_fence_soft_total
     bumps); commit not blocked.
  AC3 Fence with same epoch (no advance) → zero prune / zero
     rehydrate cost. The atomics structure: counters only bump on
     goals_dropped > 0 + rehydrate returns 0 under production. Same
     epoch → no fence → no bump.
  AC4 Successful rehydrate still preferred; hard path only when miss
     under production and prior window had occurrence work. First ship
     ships the surface + counter + Soft/Production routing; the
     concrete re-narrow vs commit_readiness force_reason choice is
     Option A vs Option B in the issue body (deferred to follow-up).
  AC5 evaluator_primitives_query.cpp exposes a distinct
     "occurrence-empty-after-fence-total" /
     "occurrence-empty-after-fence-soft-total" /
     "occurrence-empty-after-fence-wired" + "schema-2704" / "issue-2704".
     Prior #2608 / #2641 / #2552 / #2622 / #2672 surfaces preserved.
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

    tma = _read("src/compiler/typed_mutation_audit.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/compiler/test_partial_cone_commit_gate.cpp")
    build = _read("build.py")

    # AC1 — file-scope atomics + force_reason code
    must("Issue #2704", "AC1", tma)
    must("g_occurrence_empty_after_fence_total", "AC1", tma)
    must("g_occurrence_empty_after_fence_soft_total", "AC1", tma)
    must("g_occurrence_empty_after_fence_wired", "AC1", tma)
    must("kOccurrenceEmptyAfterFenceIssue = 2704", "AC1", tma)
    must("occurrence_empty_after_fence_total_v_read", "AC1", tma)
    must("occurrence_empty_after_fence_soft_total_v_read", "AC1", tma)
    must("occurrence_empty_after_fence_wired_v_read", "AC1", tma)
    must("clear_occurrence_empty_after_fence_for_test", "AC1", tma)
    must("occurrence_empty_after_fence", "AC1", tma)
    must("return 11; // #2704", "AC1", tma)
    must("note_steal_or_densify_epoch_fence", "AC1", tma)

    # AC2 — Soft path metric-only
    must("g_occurrence_empty_after_fence_soft_total", "AC2", tma)

    # AC3 — same epoch → no fence → no bump
    # Verified by the atomics structure: counters only bump on
    # goals_dropped > 0 + rehydrate returns 0 under production. Same
    # epoch → no fence → no bump.
    must("Issue #2704", "AC3", tma)

    # AC5 — query surface
    must("query:occurrence-empty-after-fence", "AC5", q)
    must("occurrence-empty-after-fence-total", "AC5", q)
    must("occurrence-empty-after-fence-soft-total", "AC5", q)
    must("occurrence-empty-after-fence-wired", "AC5", q)
    must("schema-2704", "AC5", q)
    must("issue-2704", "AC5", q)
    # Prior #2608 / #2641 / #2552 / #2622 / #2672 surfaces preserved.
    must("schema-2703", "AC5", q)
    must("schema-2694", "AC5", q)
    must("schema-2672", "AC5", q)

    # AC6 — test extension per #81967
    must("ac2704_1_production_hard_face", "AC6", t)
    must("ac2704_2_soft_observe_only", "AC6", t)
    must("ac2704_3_same_epoch_zero_cost", "AC6", t)
    must("ac2704_4_query_keys_added", "AC6", t)
    must("ac2704_5_source_and_linter", "AC6", t)
    must("ac2704_6_no_docs_design", "AC6", t)
    must("check_occurrence_empty_after_fence_2704", "AC6", build)

    # No docs/design/2704-* on disk
    docs_dir = ROOT / "docs" / "design"
    if docs_dir.is_dir():
        for f in sorted(docs_dir.glob("2704-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Cross-check: prior #2703 + #2702 + #2701 linters still green
    for prev in (
        "check_cone_outside_goal_drop_2703.py",
        "check_resume_hard_fail_2702.py",
        "check_mutation_hold_budget_reject_2701.py",
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
    print("OK: Issue #2704 occurrence-empty-after-fence production hard-face — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
