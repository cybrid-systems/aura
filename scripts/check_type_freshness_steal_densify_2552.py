#!/usr/bin/env python3
"""Issue #2552: steal/densify joint OccurrenceGoal + type_dep freshness fence.

Contract:
  AC1 note_steal_or_densify_epoch_fence API + prune goals/type_dep
  AC2 steal-complete success only (skip hard_failed)
  AC3 densify Moving path pairs escape clear + fence
  AC4 same-epoch zero cost
  AC5 multi-round soak test
  AC6 schema-2552 + counters + cmake/gate

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

    tc = _read("src/compiler/type_checker.ixx")
    etc = _read("src/compiler/evaluator_typecheck.cpp")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_type_freshness_steal_densify_2552.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("note_steal_or_densify_epoch_fence", "AC1", tc)
    must("Issue #2552", "AC1", tc)
    must("prune_occurrence_goals", "AC1", tc)
    must("occurrence_goal_steal_prune_total", "AC1", met)
    must("type_dep_steal_prune_total", "AC1", met)
    must("ac1_steal_fence_prunes_goals", "AC1", test)

    # AC2
    must("note_type_freshness_after_steal_or_densify", "AC2", efm)
    must("hard_failed", "AC2", efm)
    must("Issue #2552", "AC2", efm)
    must("ac2_hard_fail_no_fence", "AC2", test)

    # AC3
    must("note_type_freshness_after_steal_or_densify", "AC3", emb)
    must("note_escape_gate_clear_on_densify", "AC3", emb)
    must("had_moving_densify", "AC3", emb)
    must("ac3_densify_wires_fence", "AC3", test)

    # AC4
    must("same epoch", "AC4", tc + test)
    must("ac4_same_epoch_zero_cost", "AC4", test)

    # AC5
    must("ac5_multi_round_soak", "AC5", test)

    # AC6
    must("schema-2552", "AC6", q)
    must("occurrence-goal-steal-prune-total", "AC6", q)
    must("type-dep-steal-prune-total", "AC6", q)
    must("occurrence_goal_steal_prune_total", "AC6", fields)
    must("type_dep_steal_prune_total", "AC6", fields)
    must("note_type_freshness_after_steal_or_densify", "AC6", etc)
    must("test_type_freshness_steal_densify_2552", "AC6", cmake)
    must("check_type_freshness_steal_densify_2552", "AC6", build)
    must("cmd_type_freshness_steal_densify_coverage", "AC6", build)
    must("ac6_source_and_schema", "AC6", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2552 type freshness steal/densify fence — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
