#!/usr/bin/env python3
"""Issue #2973: production hard pre-densify external-root completeness.

Contract (one row per AC):
  AC1  production hard walks declared external roots BEFORE
       relocate_tracked_objects_for_moving_ and blocks when a would-move
       candidate is not slot- or pin-covered
  AC2  Soft / hard_pref<=0 stays post-move observe-only (no pre-check bump)
  AC3  slot-covered declared roots pass the pre-check
  AC4  additive pre-densify-reject / pre-densify-untracked + schema-2973
       on densify-health; post-move incomplete-remap counters preserved
  AC5  recover_moving_sticky_densify_off + #2935 inventory remain SSOT
  AC6  tests in existing Moving suite + coverage linter + no docs/design/

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

FILES = [
    "src/core/arena.ixx",
    "src/core/densify_consistency_report.h",
    "src/compiler/evaluator_primitives_obs_jit.cpp",
    "tests/core/test_moving_densify_fail_closed.cpp",
    "scripts/coverage/checks/check_moving_pre_densify_completeness_2973.py",
]


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
    report = _read("src/core/densify_consistency_report.h")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    test = _read("tests/core/test_moving_densify_fail_closed.cpp")
    build = _read("build.py")

    must("Issue #2973", "AC1 arena", arena)
    must("count_pre_densify_untracked_external_roots_", "AC1 helper", arena)
    must("g_moving_untracked_hard_abort_pref.load", "AC1 hard pref", arena)
    gate_pos = arena.find("count_pre_densify_untracked_external_roots_()")
    reloc_pos = arena.find("result.objects_moved = relocate_tracked_objects_for_moving_")
    if gate_pos == -1 or reloc_pos == -1 or gate_pos > reloc_pos:
        fails.append("AC1: pre-check must precede relocate_tracked_objects_for_moving_ call")

    must("hard_pref > 0", "AC2 Soft skip", arena)
    must("ac2973_2_soft_still_observe_only", "AC2 test", test)

    must("collect_pinned_ptrs_for_arena", "AC3 pin coverage", arena)
    must("external_root_slots_for_densify_", "AC3 slots", arena)
    must("ac2973_3_slot_covered_passes", "AC3 test", test)

    must("g_moving_pre_densify_reject_total", "AC4", report)
    must("g_moving_pre_densify_untracked_total", "AC4", report)
    must("kMovingPreDensifyCompletenessIssue = 2973", "AC4", report)
    must("pre-densify-reject-total", "AC4 densify-health", obs)
    must("pre-densify-untracked-total", "AC4 densify-health", obs)
    must("schema-2973", "AC4 densify-health", obs)
    must("schema-2495", "AC4 lineage 2495", obs)
    must("schema-2935", "AC4 lineage 2935", obs)
    must("g_moving_untracked_external_roots_total", "AC4 post-move preserved", arena)

    must("recover_moving_sticky_densify_off", "AC5 recovery", mb)
    must("register_known_moving_densify_root_slots", "AC5 inventory", mb)

    for rel in FILES:
        content = _read(rel)
        if not content:
            fails.append(f"AC6: missing file {rel}")
            continue
        if "2973" not in content and "Issue #2973" not in content:
            fails.append(f"AC6: {rel} does not cite 2973")
    must("ac2973_1_hard_blocks_before_move", "AC6", test)
    must("ac2973_6_linter_and_no_design", "AC6", test)
    must("check_moving_pre_densify_completeness_2973", "AC6 build", build)
    must("cmd_moving_pre_densify_completeness_2973", "AC6 build cmd", build)
    design_docs = sorted((ROOT / "docs" / "design").glob("2973-*")) if (ROOT / "docs" / "design").is_dir() else []
    if design_docs:
        fails.append(f"AC6: docs/design/2973-* present ({[p.name for p in design_docs]})")
    if (ROOT / "tests" / "core" / "test_issue_2973.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_2973.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        print(f"check_moving_pre_densify_completeness_2973: {len(fails)} failure(s)")
        return 1
    print("check_moving_pre_densify_completeness_2973: OK (AC1-AC6)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
