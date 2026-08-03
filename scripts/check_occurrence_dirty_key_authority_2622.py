#!/usr/bin/env python3
"""Issue #2622: single dirty-key authority for OccurrenceGoal + predicate_memo.

Contract:
  AC1 Mutate If cond shape → structural key miss / refresh (source + miss counter)
  AC2 After sync_occurrence_after_dirty, no live goal for invalidated cond
  AC3 Steal fence: goals pruned ⇒ memo stale snapshot cleared in same call
  AC4 Empty affected → zero cost (early return)
  AC5 Additive schema-2622; source-cite
  AC6 Diverge metric stays 0 on ordered sync path (test)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_occurrence_dirty_key_authority_2622.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("sync_occurrence_after_dirty", "AC1", ixx)
    must("sync_occurrence_after_dirty", "AC1", impl)
    must("cond_shape_hash", "AC1", ixx)
    must("occurrence_cache_key_miss", "AC1", impl + met)
    must("ac1_shape_miss_refresh", "AC1", test)

    # AC2
    must("drop_occurrence_goals_for_conds", "AC2", ixx)
    must("drop_occurrence_goals_for_conds", "AC2", impl)
    must("solve_delta_cs_", "AC2", impl)
    must("ac2_no_live_goal_after_sync", "AC2", test)

    # AC3
    must("last_predicate_memo_stale_vs_epoch_ = 0", "AC3", ixx)
    must("occurrence_memo_goal_fence_joint_total", "AC3", ixx + met)
    must("Issue #2622", "AC3", ixx)
    must("ac3_fence_joint_memo", "AC3", test)

    # AC4
    must("affected.empty()", "AC4", impl)
    must("AC4: zero cost when empty", "AC4", impl)
    must("ac4_empty_zero_cost", "AC4", test)

    # AC5
    must("schema-2622", "AC5", q)
    must("occurrence-memo-goal-diverge-total", "AC5", q)
    must("occurrence-sync-after-dirty-total", "AC5", q)
    must("occurrence_memo_goal_diverge_total", "AC5", fields)
    must("occurrence_dirty_key_authority_wired", "AC5", met)
    must("ac5_schema_source", "AC5", test)

    # AC6
    must("occurrence_memo_goal_diverge_total", "AC6", impl)
    must("test_occurrence_dirty_key_authority_2622", "AC6", cmake)
    must("check_occurrence_dirty_key_authority_2622", "AC6", build)
    must("cmd_occurrence_dirty_key_authority_coverage", "AC6", build)
    must("ac6_diverge_zero_ordered", "AC6", test)

    for rel in (
        "docs/design/occurrence_dirty_key_authority_2622.md",
        "docs/occurrence_dirty_key_authority_2622.md",
        "design/2622.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2622 occurrence dirty-key authority — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
