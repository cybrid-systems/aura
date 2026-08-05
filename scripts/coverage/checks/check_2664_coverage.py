#!/usr/bin/env python3
"""Issue #2664: production-default hard-fail on untracked external roots after densify
(close false-safety).

Contract (one row per AC):
  AC1 arena.ixx if-block at objects_moved > 0 && untracked_kept_count > 0
     now OR-folds production_defaults_active() into the hard-fail branch
     (alongside the pre-existing g_moving_untracked_hard_abort_pref > 0
     env=hard check). Closes the Soft-path false-safety gap where densify
     continued with metrics suppressed but no Agent-visible hard deny.
  AC2 Agent-visible hard-fail counter
     (g_moving_incomplete_remap_densify_hard_fail_total) declared in
     arena.ixx + bumped inside the if-block.
  AC3 Soft / dev_off / tests retain observe-only (the existing #2595
     Soft path comment explicitly documents this retention).
  AC4 AURA_MOVING_UNTRACKED=hard still aborts under production
     (pre-existing env=hard branch preserved; production OR-folds).
  AC5 Phase 5 gate source-cite (evaluator_mutation_boundary.cpp reads
     moving_blocked_precondition via densify_consistency.overall_ok()).
  AC6 tests/core/test_moving_densify_fail_closed.cpp extended with
     #2664 AC1-AC6 source-cite block + wired in run_test_*.
  AC7 build.py wires check_2664_coverage into the gate after
     check_moving_untracked_production_hard_2596.
  AC8 cross-check: check_moving_untracked_production_hard_2596 +
     check_stamp_resolve_coverage still green (no regression).

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

    arena = _read("src/core/arena.ixx")
    _read("src/compiler/evaluator_mutation_boundary.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")

    # AC1 — production-default hard-fail (encoded via #2596 pref > 0)
    must("Issue #2664", "AC1", arena)
    must("g_moving_untracked_hard_abort_pref", "AC1", arena)
    must("hard_pref > 0", "AC1", arena)
    must("Agent-visible hard-fail counter", "AC1", arena)

    # AC2 — Agent-visible hard-fail counter
    must("g_moving_incomplete_remap_densify_hard_fail_total{0}", "AC2", arena)
    must("g_moving_incomplete_remap_densify_hard_fail_total.fetch_add", "AC2", arena)
    must("g_moving_incomplete_remap_densify_hard_fail_total", "AC2", arena)

    # AC3 — Soft / dev_off / tests observe-only retention
    must("Soft / dev_off / tests retain observe-only", "AC3", arena)

    # AC4 — env=hard preserved + counter bumps in same branch
    must("hard_pref > 0", "AC4", arena)
    must("g_moving_incomplete_remap_densify_hard_fail_total.fetch_add", "AC4", arena)

    # AC5 — Phase 5 gate source-cite (moving_blocked_precondition in arena.ixx)
    must("moving_blocked_precondition", "AC5", arena)

    # AC6 — test file extension
    must("ac2664_1_production_default_hard_fail", "AC6", test)
    must("ac2664_2_hard_fail_counter", "AC6", test)
    must("ac2664_3_soft_observe_only", "AC6", test)
    must("ac2664_4_env_hard_still_aborts", "AC6", test)
    must("ac2664_5_phase5_gate_source_cite", "AC6", test)
    must("ac2664_6_coverage_linter_wired", "AC6", test)
    must("Issue #2664", "AC6", test)

    # AC7 — build.py wires the linter
    must("check_2664_coverage", "AC7", build)

    # Cross-check: check_moving_untracked_production_hard_2596 still green
    r1 = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_moving_untracked_production_hard_2596.py"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r1.returncode != 0:
        fails.append(f"check_moving_untracked_production_hard_2596 regression:\n{r1.stdout}\n{r1.stderr}")

    # Cross-check: stamp-resolve --strict still green
    r2 = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_stamp_resolve_coverage.py"),
            "--strict",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r2.returncode != 0:
        fails.append(f"check_stamp_resolve_coverage --strict regression:\n{r2.stdout}\n{r2.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2664 production-default hard-fail coverage — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
