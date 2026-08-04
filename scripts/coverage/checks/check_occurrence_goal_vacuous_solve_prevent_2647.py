#!/usr/bin/env python3
"""Issue #2647: live OccurrenceGoal + empty dirty must not vacuous-SOLVED.

Contract:
  AC1 forced reverify path in solve_delta_impl when dirty_count_==0 + live goals
  AC2 metrics occurrence_goal_forced_reverify / vacuous_solve_prevented
  AC3 query schema-2647 keys
  AC4 unit test present
  AC5 drift miss export (type_repair_occurrence_replay_miss)
  AC6 build.py gate wired

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    impl = _read("src/compiler/type_checker_impl.cpp")
    met = _read("src/compiler/observability_metrics.h")
    fields = _read("src/compiler/compiler_metrics_fields.inc")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/compiler/test_occurrence_goal_vacuous_solve_prevent.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    must("#2647" in impl, "AC1: type_checker_impl cites #2647")
    must("occurrence_goal_forced_reverify_total" in impl, "AC1: forced_reverify counter bump")
    must(
        "occurrence_goal_vacuous_solve_prevented_total" in impl,
        "AC1: vacuous_solve_prevented counter bump",
    )
    must("try_goal_priority_reverify_before_full" in impl, "AC1: goal-priority reverify called")
    # Early dirty_count_==0 path must still force reverify under goals.
    must("dirty_count_ == 0" in impl, "AC1: empty-dirty early path present")

    must("occurrence_goal_forced_reverify_total" in met, "AC2: metrics field forced_reverify")
    must(
        "occurrence_goal_vacuous_solve_prevented_total" in met,
        "AC2: metrics field vacuous_prevented",
    )
    must(
        "occurrence_goal_forced_reverify_total" in fields,
        "AC2: fields.inc forced_reverify",
    )
    must(
        "occurrence_goal_vacuous_solve_prevented_total" in fields,
        "AC2: fields.inc vacuous_prevented",
    )

    must("schema-2647" in q, "AC3: query schema-2647")
    must("occurrence-goal-forced-reverify-total" in q, "AC3: query forced-reverify key")
    must(
        "occurrence-goal-vacuous-solve-prevented-total" in q,
        "AC3: query vacuous-prevented key",
    )
    must("issue-2647" in q, "AC3: issue-2647 query key")

    must("AC1" in test and "AC4" in test and "AC6" in test, "AC4: unit test ACs present")
    must(
        "solve_delta_occurrence" in test and "mark_clean" in test,
        "AC4: hermetic SDO + mark_clean",
    )
    must(
        "test_occurrence_goal_vacuous_solve_prevent.cpp" in cmake,
        "AC4: cmake registers test",
    )

    must(
        "type_repair_occurrence_replay_miss_count" in impl,
        "AC5: drift miss export path",
    )

    must(
        "check_occurrence_goal_vacuous_solve_prevent_2647" in build,
        "AC6: build.py wires linter",
    )
    must(
        "cmd_occurrence_goal_vacuous_solve_prevent_coverage" in build
        or "occurrence_goal_vacuous_solve_prevent" in build,
        "AC6: build.py coverage cmd or reference",
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2647 occurrence goal vacuous-solve prevent — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
