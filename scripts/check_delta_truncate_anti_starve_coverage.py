#!/usr/bin/env python3
"""
Linter for #2318 — anti-starvation streak gate for consecutive truncated
delta solves. Closes the long-session leak where under multi-round
Agent mutate, the bounded solve_delta clean-reverify can truncate
repeatedly without the rare TIMEOUT trigger (#2277) escalating to a
full solve. After #2310-#2317 correctness fixes, the streak gate is
the missing piece — N consecutive truncates force one full
ConstraintSystem::solve() (mirror #2277 escalation body).

Verifies the implementation is wired correctly:
  - observability_metrics.h: new per-CompilerMetrics counters
    (delta_reverify_truncate_streak + delta_truncate_force_full_solve_total
    + delta_truncate_streak_threshold + delta_truncate_anti_starve_wired)
  - type_checker.ixx: streak field in ConstraintSystem + env accessor
    AURA_DELTA_TRUNCATE_STREAK_FULL (default 2) + check_truncate_anti_starve
    method declaration
  - type_checker_impl.cpp: MetricsAccess struct extended with new fields +
    solve_delta calls streak check + force full solve on threshold
  - evaluator_primitives_query.cpp: query:type-incremental-fidelity-stats
    extended with delta-reverify-truncate-streak + delta-truncate-force-
    full-solve-total + delta-truncate-streak-threshold + delta-truncate-
    anti-starve-wired + schema-2318 / issue-2318
  - tests/compiler/test_solve_delta_unresolved_export_2107.cpp: extended
    with ac2318_* test functions + Issue #2318 cite

Exit 0 on success, 1 on any failure. Run as part of the ship loop:
    python3 scripts/check_delta_truncate_anti_starve_coverage.py
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
        # observability_metrics.h: new per-CompilerMetrics counters
        (
            ROOT / "src/compiler/observability_metrics.h",
            "delta_reverify_truncate_streak{0}; // #2318",
            "observability_metrics.h: delta_reverify_truncate_streak counter",
        ),
        (
            ROOT / "src/compiler/observability_metrics.h",
            "delta_truncate_force_full_solve_total{0}; // #2318",
            "observability_metrics.h: delta_truncate_force_full_solve_total counter",
        ),
        (
            ROOT / "src/compiler/observability_metrics.h",
            "delta_truncate_streak_threshold{0}; // #2318",
            "observability_metrics.h: delta_truncate_streak_threshold counter",
        ),
        (
            ROOT / "src/compiler/observability_metrics.h",
            "delta_truncate_anti_starve_wired{0}; // #2318",
            "observability_metrics.h: delta_truncate_anti_starve_wired sentinel",
        ),
        # type_checker.ixx: streak field + env accessor + method declaration
        (
            ROOT / "src/compiler/type_checker.ixx",
            "truncate_streak_ = 0;",
            "type_checker.ixx: truncate_streak_ field init",
        ),
        (
            ROOT / "src/compiler/type_checker.ixx",
            "AURA_DELTA_TRUNCATE_STREAK_FULL",
            "type_checker.ixx: AURA_DELTA_TRUNCATE_STREAK_FULL env var",
        ),
        (
            ROOT / "src/compiler/type_checker.ixx",
            "delta_truncate_streak_threshold()",
            "type_checker.ixx: delta_truncate_streak_threshold() accessor",
        ),
        (
            ROOT / "src/compiler/type_checker.ixx",
            "check_truncate_anti_starve",
            "type_checker.ixx: check_truncate_anti_starve method",
        ),
        (ROOT / "src/compiler/type_checker.ixx", "Issue #2318", "type_checker.ixx: cites Issue #2318"),
        # type_checker_impl.cpp: solve_delta modified
        (
            ROOT / "src/compiler/type_checker_impl.cpp",
            "delta_truncate_streak_threshold",
            "type_checker_impl.cpp: solve_delta reads threshold",
        ),
        (
            ROOT / "src/compiler/type_checker_impl.cpp",
            "truncate_streak_",
            "type_checker_impl.cpp: solve_delta manages truncate_streak_",
        ),
        (
            ROOT / "src/compiler/type_checker_impl.cpp",
            "return solve(unresolved_out);",
            "type_checker_impl.cpp: full solve call present",
        ),
        (ROOT / "src/compiler/type_checker_impl.cpp", "Issue #2318", "type_checker_impl.cpp: cites Issue #2318"),
        # evaluator_primitives_query.cpp: query keys
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "delta-reverify-truncate-streak",
            "query: delta-reverify-truncate-streak key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "delta-truncate-force-full-solve-total",
            "query: delta-truncate-force-full-solve-total key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "delta-truncate-streak-threshold",
            "query: delta-truncate-streak-threshold key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_query.cpp",
            "delta-truncate-anti-starve-wired",
            "query: delta-truncate-anti-starve-wired key",
        ),
        (ROOT / "src/compiler/evaluator_primitives_query.cpp", "schema-2318", "query: schema-2318 sentinel"),
        (ROOT / "src/compiler/evaluator_primitives_query.cpp", "issue-2318", "query: issue-2318 sentinel"),
        # test file
        (
            ROOT / "tests/compiler/test_solve_delta_unresolved_export_2107.cpp",
            "ac2318_streak_counter",
            "test: ac2318_streak_counter function present",
        ),
        (
            ROOT / "tests/compiler/test_solve_delta_unresolved_export_2107.cpp",
            "ac2318_force_full_solve",
            "test: ac2318_force_full_solve function present",
        ),
        (
            ROOT / "tests/compiler/test_solve_delta_unresolved_export_2107.cpp",
            "ac2318_query_keys",
            "test: ac2318_query_keys function present",
        ),
        (
            ROOT / "tests/compiler/test_solve_delta_unresolved_export_2107.cpp",
            "ac2318_source_cite_rows",
            "test: ac2318_source_cite_rows function present",
        ),
        (ROOT / "tests/compiler/test_solve_delta_unresolved_export_2107.cpp", "Issue #2318", "test: cites Issue #2318"),
        # Linter self-reference (sanity)
        (
            ROOT / "scripts/check_delta_truncate_anti_starve_coverage.py",
            "anti-starvation streak gate",
            "linter self-reference",
        ),
    ]

    failed = 0
    for file, needle, label in checks:
        if not must_contain(file, needle, label):
            failed += 1

    if failed == 0:
        print(f"\nAll {len(checks)} #2318 checks passed.")
        return 0
    print(f"\n{failed} of {len(checks)} check(s) failed.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
