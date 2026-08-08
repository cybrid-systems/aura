#!/usr/bin/env python3
"""Issue #2716: wire occurrence hard-faces into active commit path.

Closes the #2703 / #2704 residual: #2703 / #2704 shipped force_reason
codes + counters + Soft/Production routing surface for
cone_outside_goal_drop (code 10) and occurrence_empty_after_fence
(code 11), but commit_readiness / outermost boundary still did NOT
actively force a full ConstraintSystem::solve() recover or hard-reject
based on these counters. Production Agents could still pass a green
path while occurrence narrowing was silently gone (half-green typed
mutate under multi-delta / multi-fiber).

#2716 wires the active branch: under production/Full + face hit
(counter > 0), commit_readiness hard-rejects with the new
force_reasons. Soft / baseline=0: counter-only. Option A's "one full
ConstraintSystem::solve() recover" half is deferred (thin ship).

Contract rows (AC1–AC6 from the test file):

  AC1: production + face hit → commit hard-rejects with new
       force_reasons (code 10 / 11).
  AC2: Soft / baseline=0 → counter-only, no reject.
  AC3: Quiet path (no face hit) → zero extra cost (single relaxed
       load of face counters when in prod/Full; no atomics otherwise).
  AC4: Additive only — preserve #2703 / #2704 / #2621 / #2458 / #2608
       query keys and schemas.
  AC5: Additive query keys only — occurrence-hard-face-full-solve-
       recover-total + schema-2716 + issue-2716 sentinels.
  AC6: source-cite + linter + no docs/design/.

Exit 0 = all contract rows satisfied.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _self_test() -> int:
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "check_occurrence_hard_face_commit_2716.py")],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print(f"--self-test FAILED:\n{r.stdout}\n{r.stderr}", file=sys.stderr)
        return 1
    print(f"--self-test OK: {r.stdout.strip()}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--self-test", action="store_true", help="Run self-test on this linter")
    args = p.parse_args()

    if args.self_test:
        return _self_test()

    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    t = _read("tests/compiler/test_partial_cone_commit_gate.cpp")
    build = _read("build.py")

    # AC1 — production + face hit → commit hard-rejects with new
    # force_reasons. Force_reason codes 10 + 11 + active branch.
    must("Issue #2716", "AC1", tma)
    must("cone_outside_goal_drop_face", "AC1", tma)
    must("occurrence_empty_after_fence_face", "AC1", tma)
    must("occurrence_face_hard", "AC1", tma)
    must("return 10; // #2703", "AC1", tma)
    must("return 11; // #2704", "AC1", tma)
    must("if (in.occurrence_face_hard)", "AC1", tma)
    must('set("cone_outside_goal_drop", false, 800)', "AC1", tma)
    must('set("occurrence_empty_after_fence", false, 850)', "AC1", tma)

    # AC2 — Soft / baseline=0 → counter-only, no reject.
    must("in.occurrence_face_hard = face_hard", "AC2", tma)
    must("const bool face_hard = prod || full", "AC2", tma)
    must("g_cone_outside_goal_drop_soft_total", "AC2", tma)
    must("g_occurrence_empty_after_fence_soft_total", "AC2", tma)

    # AC3 — Quiet path zero extra cost (single relaxed load).
    must("cone_outside_goal_drop_total_v_read() > 0", "AC3", tma)
    must("occurrence_empty_after_fence_total_v_read() > 0", "AC3", tma)
    must("if (face_hard)", "AC3", tma)

    # AC4 — Additive only (no regression on #2703 / #2704 / #2621 / #2458 / #2608).
    must("kConeOutsideGoalDropIssue = 2703", "AC4", tma)
    must("kOccurrenceEmptyAfterFenceIssue = 2704", "AC4", tma)
    must("Issue #2621", "AC4", tma)
    must("Issue #2458", "AC4", tma)
    must("g_occurrence_hard_face_full_solve_recover_total", "AC4", tma)

    # AC5 — Additive query keys (kebab + schema/issue sentinels).
    must_key("occurrence-hard-face-full-solve-recover-total", "AC5", q)
    must_key("occurrence-hard-face-full-solve-recover-wired", "AC5", q)
    must_key("schema-2716", "AC5", q)
    must_key("issue-2716", "AC5", q)
    # Regression on prior surfaces.
    must_key("cone-outside-goal-drop-total", "AC5", q)
    must_key("occurrence-empty-after-fence-total", "AC5", q)
    must_key("schema-2703", "AC5", q)
    must_key("schema-2704", "AC5", q)

    # AC6 — source-cite + linter + build.py + no docs/design/.
    must("ac2716_1_production_hard_reject_on_face_hit", "AC6", t)
    must("ac2716_2_soft_counter_only", "AC6", t)
    must("ac2716_3_quiet_path_zero_cost", "AC6", t)
    must("ac2716_4_additive_no_regression", "AC6", t)
    must("ac2716_5_query_keys_added", "AC6", t)
    must("ac2716_6_source_and_linter", "AC6", t)
    must("check_occurrence_hard_face_commit_2716", "AC6", build)
    if _read("docs/design/2716-occurrence-hard-face-commit.md"):
        fails.append("AC6: docs/design/2716-* exists (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2716 occurrence hard-faces active commit path — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
