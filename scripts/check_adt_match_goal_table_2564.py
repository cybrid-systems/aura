#!/usr/bin/env python3
"""Issue #2564: ADT match exhaustiveness goal table coverage.

Contract:
  AC1 note_adt_match_goal + invalidate → reverify roots; partial drain path
  AC2 empty table zero invalidate/reverify
  AC3 AURA_ADT_GOAL_TABLE_CAP + cap drop
  AC4 schema-2564 + fidelity keys
  AC5 #2223/#2264 hard-gate retained; test + cmake + gate

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

    tixx = _read("src/compiler/type_checker.ixx")
    tci = _read("src/compiler/type_checker_impl.cpp")
    om = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    test = _read("tests/compiler/test_adt_match_goal_table_2564.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1
    must("AdtMatchGoal", "AC1", tixx)
    must("note_adt_match_goal", "AC1", tixx)
    must("note_adt_match_goal", "AC1", tci)
    must("invalidate_adt_goals_for", "AC1", tixx)
    must("drain_adt_reverify_roots", "AC1", tci)
    must("absorb_pending_adt_reverify_roots", "AC1", tci)
    must("g_adt_reverify_pending_tls", "AC1", tci)
    must("ac1_note_invalidate_reverify", "AC1", test)

    # AC2
    must("zero cost when no goals", "AC2", tci)
    must("ac2_zero_work", "AC2", test)

    # AC3
    must("AURA_ADT_GOAL_TABLE_CAP", "AC3", tci)
    must("adt_goal_cap_drop_total", "AC3", om)
    must("ac3_cap", "AC3", test)

    # AC4
    must("schema-2564", "AC4", q)
    must("adt-goal-table-size", "AC4", q)
    must("adt-goal-invalidate-total", "AC4", q)
    must("adt-reverify-root-total", "AC4", q)
    must("ac4_schema", "AC4", test)

    # AC5
    must("match_sites", "AC5", emb)
    must("check_match_exhaustiveness", "AC5", tci)
    must("invalidate_match_exhaust_for_adt_type", "AC5", tci)
    must("test_adt_match_goal_table_2564", "AC5", cmake)
    must("check_adt_match_goal_table_2564", "AC5", build)
    must("cmd_adt_match_goal_table_coverage", "AC5", build)
    must("ac5_hard_gate_retained", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2564 ADT match goal table — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
