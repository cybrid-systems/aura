#!/usr/bin/env python3
"""
Linter for #2321 — replay-time re-validate OccurrenceGoal.refined against
current Union-Find binding of goal.var (drift detection). Closes the gap
where under multi-round mutate, the var's binding can shift across
epochs while the recorded refined stays put — leaving stale narrowed
types that don't match the live binding. After #2278 (epoch-scoped table)
and #2307 (sole-authority refactor), the missing piece is a re-validate
gate at replay time that drops drifted goals + bumps a new
occurrence_goal_refined_drift_total counter.

Verifies the implementation is wired correctly:
  - observability_metrics.h: new per-CompilerMetrics counter
    (occurrence_goal_refined_drift_total) — distinct from the
    epoch-prune occurrence_goal_stale_drop_total
  - type_checker_impl.cpp: solve_delta_occurrence loop extended with
    cs.find(goal.var) → cs.consistent_unify(cur, goal.refined) ||
    cs.consistent_unify(goal.refined, cur) drift gate; bumps both
    occurrence_goal_refined_drift_total + occurrence_goal_stale_drop_total
    on drift; uses bidirectional consistent_unify so gradual Dynamic
    goals survive (no strict EQUAL-only check)
  - evaluator_primitives_query.cpp: query:type-incremental-fidelity-stats
    extended with occurrence-goal-refined-drift-total +
    occurrence_goal_refined_drift_total (snake alias) +
    occurrence-goal-drift-wired sentinel + schema-2321 + issue-2321
  - tests/compiler/test_occurrence_goal_epoch_table_2278.cpp: extended
    with ac8_2321_counter_initialized + ac9_2321_schema_sentinels +
    ac10_2321_gradual_dynamic_no_drift functions + Issue #2321 cite

Exit 0 on success, 1 on any failure. Run as part of the ship loop:
    python3 scripts/check_occurrence_goal_drift_coverage.py
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def must_contain(file: Path, needle: str, label: str) -> bool:
    if not file.exists():
        print(f"FAIL {label}: file {file} does not exist")
        return False
    content = read(file)
    if needle in content:
        print(f"OK  {label}")
        return True
    print(f"FAIL {label}: '{needle}' not found in {file}")
    return False


def main() -> int:
    checks = [
        # observability_metrics.h: new per-CompilerMetrics counter
        (
            ROOT / "src/compiler/observability_metrics.h",
            "occurrence_goal_refined_drift_total{0}; // #2321",
            "observability_metrics.h: occurrence_goal_refined_drift_total counter",
        ),
        (
            ROOT / "src/compiler/observability_metrics.h",
            "Issue #2321: refined-drift counter",
            "observability_metrics.h: cites Issue #2321",
        ),
        # type_checker_impl.cpp: solve_delta_occurrence re-validate loop
        (
            ROOT / "src/compiler/type_checker_impl.cpp",
            "occurrence_goal_refined_drift_total.fetch_add(",
            "type_checker_impl.cpp: drift counter fetch_add",
        ),
        (
            ROOT / "src/compiler/type_checker_impl.cpp",
            "auto cur = cs.find(goal.var);",
            "type_checker_impl.cpp: cs.find(goal.var) for cur binding",
        ),
        (
            ROOT / "src/compiler/type_checker_impl.cpp",
            "!cs.consistent_unify(goal.refined, cur)",
            "type_checker_impl.cpp: bidirectional consistent_unify drift gate",
        ),
        (
            ROOT / "src/compiler/type_checker_impl.cpp",
            "occurrence_goal_stale_drop_total.fetch_add(",
            "type_checker_impl.cpp: also bumps stale_drop_total on drift",
        ),
        (
            ROOT / "src/compiler/type_checker_impl.cpp",
            "Issue #2321: re-validate refined",
            "type_checker_impl.cpp: cites Issue #2321 in drift gate comment",
        ),
        (
            ROOT / "src/compiler/type_checker_impl.cpp",
            "std::size_t drifted = 0;",
            "type_checker_impl.cpp: local drifted counter",
        ),
        # evaluator_primitives_query.cpp: query keys + sentinels
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "occurrence-goal-refined-drift-total",
            "query: occurrence-goal-refined-drift-total kebab key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "occurrence_goal_refined_drift_total",
            "query: occurrence_goal_refined_drift_total snake alias",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "occurrence-goal-drift-wired",
            "query: occurrence-goal-drift-wired sentinel",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "schema-2321",
            "query: schema-2321 sentinel",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "issue-2321",
            "query: issue-2321 sentinel",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "Issue #2321: OccurrenceGoal refined-drift observability",
            "query: cites Issue #2321 in comment block",
        ),
        # test file: ac2321_* functions + Issue #2321 cite
        (
            ROOT / "tests/compiler/test_occurrence_goal_epoch_table_2278.cpp",
            "ac8_2321_counter_initialized",
            "test: ac8_2321_counter_initialized function present",
        ),
        (
            ROOT / "tests/compiler/test_occurrence_goal_epoch_table_2278.cpp",
            "ac9_2321_schema_sentinels",
            "test: ac9_2321_schema_sentinels function present",
        ),
        (
            ROOT / "tests/compiler/test_occurrence_goal_epoch_table_2278.cpp",
            "ac10_2321_gradual_dynamic_no_drift",
            "test: ac10_2321_gradual_dynamic_no_drift function present",
        ),
        (
            ROOT / "tests/compiler/test_occurrence_goal_epoch_table_2278.cpp",
            "Issue #2321 (Refine #2278 + #2307)",
            "test: cites Issue #2321 in header AC list",
        ),
        # Linter self-reference (sanity)
        (
            ROOT / "scripts/check_occurrence_goal_drift_coverage.py",
            "replay-time re-validate OccurrenceGoal.refined",
            "linter self-reference",
        ),
    ]

    failed = 0
    for file, needle, label in checks:
        if not must_contain(file, needle, label):
            failed += 1

    if failed == 0:
        print(f"\nAll {len(checks)} #2321 checks passed.")
        return 0
    print(f"\n{failed} of {len(checks)} check(s) failed.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
