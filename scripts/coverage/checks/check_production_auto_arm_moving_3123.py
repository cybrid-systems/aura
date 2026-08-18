#!/usr/bin/env python3
"""Issue #3123: production Moving densify auto-arm + sticky-clear discipline.

When production pack is active, frag ≥ kAutoMovingCompactThreshold, pins
and MutationBoundary / EnvFrame guards are quiet, and Moving is on: the
existing auto-compact path may request live_compact(Moving). Soft/sandbox
never auto-arms. Sticky densify-off clears only after a healthy Moving
window. Agent surface exposes auto-arm + last clear reason.

Contract:
  AC1 Production pack + high frag + zero pins/guards → auto-arm helper
  AC2 Production untracked kept > 0 remains fail-closed + sticky
  AC3 Sticky clears only after healthy Moving (complete / zero untracked)
  AC4 Soft/Force stay non-moving; no new process-wide lock
  AC5 Extend test_arena_moving_densify_health; no test_issue_3123.cpp

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

    arena = _read("src/core/arena.ixx")
    hh = _read("src/core/moving_densify_health.hh")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    phase5 = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t = _read("tests/compiler/test_arena_moving_densify_health.cpp")
    build = _read("build.py")

    must("Issue #3123", "AC1 arena", arena)
    must("should_production_auto_arm_moving", "AC1 helper", arena)
    must("kAutoMovingCompactThreshold", "AC1 threshold", arena)
    must("live_compact(LiveCompactMode::Moving)", "AC1 auto path Moving", arena)
    must("moving_compact_feature_enabled", "AC3 feature vs sticky split", arena)
    must("g_production_auto_arm_moving_pref", "AC1 pack pref", arena)
    must("sandbox_dev_off_for_auto_arm", "AC1 Soft/sandbox gate", arena)
    must("live_pin_count()", "AC1 pin quiet", arena)
    must("active_guard_depth()", "AC1 EnvFrame quiet", arena)
    must("arena_mutation_boundary_depth()", "AC1 boundary quiet", arena)
    must("ac3123_1_production_auto_arm_soft_never", "AC1 test", t)

    must("untracked_kept_count > 0", "AC2 fail-closed", arena)
    must("g_moving_incomplete_remap_sticky_densify_off", "AC2 sticky", arena)
    must("ac3123_2_untracked_fail_closed_sticky", "AC2 test", t)

    must("kStickyClearHealthyWindow", "AC3 healthy reason", arena)
    must("kStickyClearZeroMoveClean", "AC3 zero-move reason", arena)
    must("clear_moving_incomplete_remap_sticky_densify_off_reason", "AC3 reason helper", arena)
    must("root_remap_stable_ref_fail_total == 0", "AC3 after RootRemap", arena)
    must("kStickyClearPhase5Green", "AC3 Phase-5 reason", phase5)
    must("densify_untracked_kept == 0", "AC3 Phase-5 untracked", phase5)
    must("ac3123_3_sticky_clears_only_on_healthy", "AC3 test", t)

    must("ac3123_4_soft_force_unchanged", "AC4 test", t)
    must("Soft/sandbox never auto-arms", "AC4 comment", arena)

    must("kProductionAutoArmMovingIssue = 3123", "AC5 health stamp", hh)
    must("production-auto-arm-total", "AC5 query auto-arm", q)
    must("sticky-last-clear-reason", "AC5 query reason", q)
    must("schema-3123", "AC5 schema", q)
    must("note_production_auto_arm", "AC5 health note", hh)
    must("ac3123_5_agent_surface_and_linter", "AC5 test", t)
    must("check_production_auto_arm_moving_3123", "AC5 build.py", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3123.cpp").is_file():
        fails.append("AC5: test_issue_3123.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3123.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3123.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3123-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3123 production Moving auto-arm + sticky-clear — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
