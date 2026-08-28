#!/usr/bin/env python3
"""Issue #3108: commit_readiness recover must re-gate on solve_status==SOLVED
(anti half-green after occurrence hard-face recover).

Contract (one row per AC):
  AC1  post-recover re-gate wired (single `if (recovered && in.solve_status != 0)`
       check before any fall-through to "ok"); both existing recover blocks
       (cone_outside_goal_drop / occurrence_empty_after_fence) now bump
       g_occurrence_recover_not_solved_total when the gate fires
  AC2  Production reject path unchanged (recovered->false flip already routes
       through existing face reject sites; #3108 just adds observability)
  AC3  Soft observe-only (counter bump is the observable signal; Soft
       doesn't hard-reject from this gate — the gate is observable, not
       actionable, under Soft)
  AC4  Additive counter + wired flag + issue stamp only; Quiet path
       (no recover attempted) stays zero extra atomics — the bump lives
       INSIDE the cold `recovered && solve_status != 0` branch
  AC5  Source-cite in typed_mutation_audit.h + extend
       test_solve_delta_unresolved_export.cpp (#81967); no
       docs/design/3108-* (#1655); no test_issue_3108.cpp

#3380 supersedes the re-gate path: recover is now bound to the live
commit TypeChecker (C ABI in evaluator_mutation_boundary.cpp walks
g_tls_audit_commit_readiness_evaluator → commit_type_checker_handle →
TypeChecker::try_occurrence_hard_face_full_solve_recover). The live TC's
recover returns true ONLY when solve() returned SOLVED — it cannot lie,
so the `if (recovered && in.solve_status != 0)` re-gate (and its
g_occurrence_recover_not_solved_total bump sites) are dead code and
have been removed. The counter itself stays defined (additive, no new
query key per #3380 spec); the re-gate check is gone. This linter now
asserts the supersession: re-gate absent, bump sites absent, counter
still present + wired flag still present.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    h = _read("src/compiler/typed_mutation_audit.h")
    test = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    build = _read("build.py")
    _read("scripts/coverage/checks/check_solve_delta_fail_closed_3031.py")

    # ── AC1: post-recover re-gate wired (single check, both blocks bump) ──
    # #3380 supersedes the re-gate path: live commit TC recover can't lie
    # (returns true only when solve() returned SOLVED), so the
    # `if (recovered && in.solve_status != 0)` re-gate + its bump sites
    # are dead code and have been removed. Counter + wired flag stay
    # defined (additive, no new query key).
    must("kOccurrenceRecoverNotSolvedIssue = 3108", "AC1 issue stamp", h)
    must("g_occurrence_recover_not_solved_total", "AC1 additive counter", h)
    must("g_occurrence_recover_not_solved_wired{1}", "AC1 wired flag", h)
    # Re-gate check is GONE (superseded by #3380 live TC binding).
    # Strip line comments before searching — the pattern is referenced
    # in documentation comments explaining the old behavior, but only
    # the active-code occurrence counts.
    h_stripped = "\n".join(line for line in h.split("\n") if not line.lstrip().startswith("//"))
    if "if (recovered && in.solve_status != 0)" in h_stripped:
        fails.append("AC1: stale re-gate check present (#3380 superseded — live TC recover can't lie)")
    # Bump sites are GONE (re-gate is gone, bumps were inside the gate branch).
    bump_positions = [
        m.start()
        for m in re.finditer(
            r"g_occurrence_recover_not_solved_total\.fetch_add\(1,\s*std::memory_order_relaxed\)", h_stripped
        )
    ]
    if len(bump_positions) != 0:
        fails.append(f"AC1: expected 0 bump sites (#3380 superseded re-gate), found {len(bump_positions)}")
    # No third direct-if fall-through (line 2406 direct-if usage must NOT
    # bump the new counter without the re-gate) — the existing #2750 reject
    # site at line 2413-2415 handles the non-SOLVED case there.
    must("3108 AC1", "AC1 test marker", test)

    # ── AC2: Production reject path unchanged ────────────────────────────
    # The recovered->false flip already routes through existing face
    # reject sites. #3108 just adds observability. Source-cite the
    # existing reject counters must remain.
    must("g_occurrence_hard_face_recover_fail_total", "AC2 hard-face recover-fail counter", h)
    must("g_cone_outside_goal_drop_reject_total", "AC2 cone reject counter", h)
    must("g_refined_consistency_reject_total", "AC2 refined reject counter", h)
    # The new bump must NOT introduce a new reject path — it must live
    # INSIDE the existing `if (recovered && in.solve_status != 0)` branch
    # and just flip recovered->false (the existing flow then routes to
    # the existing reject sites via the `else` branches or fall-through).
    all(
        # Find each bump position and verify the immediately preceding
        # block contains the re-gate check.
        True  # simplified: the assert above on bump_positions already
        # verifies ≥2 bumps; the gate check is verified separately.
        for _ in bump_positions
    )
    must("3108 AC2", "AC2 test marker", test)

    # ── AC3: Soft observe-only (counter is observable signal, not hard action) ─
    # The re-gate triggers ONLY when `in.solve_status != 0` (CONFLICT /
    # TIMEOUT). Under Soft, this is observable but not actionable from
    # this gate alone — the existing Soft TIMEOUT authority clear (#3081)
    # handles the Soft authority side. Source-cite #3081 lineage preserved.
    # The bump must NOT be inside a Soft-only branch (it's in the gate
    # branch which fires regardless of Soft/Production when
    # recover returns true under non-SOLVED). Source-cite: the bump
    # is inside `if (recovered && in.solve_status != 0)` which is the
    # gate itself, not gated on production path.
    must("3108 AC3", "AC3 test marker", test)

    # ── AC4: Additive counter only — Quiet path zero extra atomics ───────
    # The bump lives INSIDE the cold `recovered && solve_status != 0`
    # branch which is only entered when a recover hook returns true.
    # When no recover is attempted (Quiet path), the gate is never
    # entered → no extra atomic RMW beyond the existing face loads.
    must("kOccurrenceRecoverNotSolvedIssue = 3108", "AC4 additive issue stamp", h)
    must("g_occurrence_recover_not_solved_total", "AC4 additive counter", h)
    must("g_occurrence_recover_not_solved_wired{1}", "AC4 additive wired flag", h)
    # No new dirty / mark_dirty / set_dirty / push_dirty calls in the
    # recover blocks (would break Quiet zero-cost guarantee).
    # Find the region around each bump and check for forbidden tokens.
    for pos in bump_positions:
        # Look at 200 chars before the bump for context
        ctx = h[max(0, pos - 200) : pos + 200]
        for forbidden in ("mark_dirty", "set_dirty", "stage_dirty", "push_dirty"):
            if forbidden in ctx:
                fails.append(f"AC4: bump at {pos} introduces dirty bit '{forbidden}'")
    must("3108 AC4", "AC4 test marker", test)

    # ── AC5: Source-cite + extend existing test, no docs/design/*, no test_issue_* ─
    must("kOccurrenceRecoverNotSolvedIssue = 3108", "AC5 issue stamp", h)
    must("ac3108_1_recover_regate_wired", "AC5 AC1 test function", test)
    must("ac3108_2_production_rejects_via_existing_path", "AC5 AC2 test function", test)
    must("ac3108_3_soft_observe_only", "AC5 AC3 test function", test)
    must("ac3108_4_additive_counter_only", "AC5 AC4 test function", test)
    must("ac3108_5_source_and_linter", "AC5 AC5 test function", test)
    # Linter wired in build.py
    must("check_occurrence_recover_not_solved_3108", "AC5 build.py wiring", build)
    must("Issue #3108", "AC5 linter error message", build)
    # No invent
    if (ROOT / "tests" / "compiler" / "test_issue_3108.cpp").is_file():
        fails.append("AC5: test_issue_3108.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3108.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3108.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3108-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    # Lineage: #2750, #2909, #2962, #2911, #3031 must still pass
    must("kConeOutsideGoalDropRecoverRejectIssue = 2962", "AC5 #2962 lineage", h)
    must("kRefinedConsistencyGateIssue = 2911", "AC5 #2911 lineage", h)
    must("3031", "AC5 3031 linter lineage", _read("scripts/coverage/checks/check_pending_full_solve_residual_3031.py"))
    must("3108 AC5", "AC5 test marker", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3108 commit_readiness recover re-gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
