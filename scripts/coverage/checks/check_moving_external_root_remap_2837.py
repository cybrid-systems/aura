#!/usr/bin/env python3
"""Issue #2837: Moving densify external-root slot remap + sticky densify-off.

Contract (one row per AC):
  AC1 register_external_root_slot_for_densify rewrites *slot after densify
  AC2 value-only prep register without slot → stale_unremapped / incomplete
  AC3 production hard incomplete arms sticky densify-off (Moving forced off)
  AC4 Soft / hard_pref<=0 does not arm sticky densify-off
  AC5 Soft / no-move path: slot rewrite gated on objects_moved > 0
  AC6 Agent surface schema-2837 + test extension + linter; no docs/design/

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
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")

    # AC1 — slot registration + rewrite
    must("Issue #2837", "AC1", arena)
    must("register_external_root_slot_for_densify", "AC1", arena)
    must("external_root_slots_for_densify_", "AC1", arena)
    must("g_moving_external_root_slot_remap_total", "AC1", arena)
    must("external_roots_remapped_count", "AC1", arena)
    must("last_object_remap_.find(*slot)", "AC1", arena)

    # AC2 — stale unremapped detection
    must("external_roots_stale_unremapped_count", "AC2", arena)
    must("stale_unremapped", "AC2", arena)
    must("slot_covered_old", "AC2", arena)

    # AC3 — sticky densify-off under production hard
    must("g_moving_incomplete_remap_sticky_densify_off", "AC3", arena)
    must("clear_moving_incomplete_remap_sticky_densify_off", "AC3", arena)
    must("g_moving_incomplete_remap_sticky_densify_off_total", "AC3", arena)
    # sticky forces moving_compact_enabled → 0
    must("g_moving_incomplete_remap_sticky_densify_off.load", "AC3", arena)

    # AC4 — Soft does not arm sticky (gated on hard_pref > 0)
    must("hard_pref > 0", "AC4", arena)
    must("Soft (hard_pref <= 0) does not arm sticky", "AC4", arena)

    # AC5 — no-move zero cost
    must("result.objects_moved > 0 && !external_root_slots_for_densify_.empty()", "AC5", arena)

    # AC6 — Agent surface + test + linter + no invent/design
    must("schema-2837", "AC6", obs)
    must("issue-2837", "AC6", obs)
    must("sticky-densify-off", "AC6", obs)
    must("external-root-slot-remap-total", "AC6", obs)
    must("external-root-remap-wired", "AC6", obs)
    must("ac2837_1_slot_remapped", "AC6", test)
    must("ac2837_3_sticky_densify_off_under_hard", "AC6", test)
    must("check_moving_external_root_remap_2837", "AC6", build)
    if (ROOT / "tests" / "core" / "test_issue_2837.cpp").is_file():
        fails.append("AC6: test_issue_2837.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2837-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Cross-check: #2495 / #2664 / #2682 still green
    for linter in (
        "check_moving_densify_fail_closed_2495.py",
        "check_2664_coverage.py",
        "check_moving_unified_success_2682.py",
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
    print("OK: Issue #2837 Moving external-root slot remap + sticky densify-off")
    return 0


if __name__ == "__main__":
    sys.exit(main())
