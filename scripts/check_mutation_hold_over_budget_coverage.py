#!/usr/bin/env python3
"""
Linter for #2313 — MutationBoundary hold-budget + over-budget cooperative
safepoint (tail latency). Closes the production tail-latency hole where
outermost MutationBoundaryGuard can hold exclusive workspace_mtx_ + GcDefer
indefinitely under sustained multi-agent load (steal scoring already penalizes
long last_hold_us #2253 but there's no preventive budget signal).

Verifies the implementation is wired correctly:
  - evaluator_mutation_boundary.cpp dtor bumps
    mutation_hold_over_budget_total when uus > mutation_hold_budget_us()
  - evaluator_mutation_boundary.cpp exposes
    mutation_hold_budget_us() env-driven accessor (AURA_MUTATION_HOLD_BUDGET_US,
    default 100_000 µs)
  - observability_metrics.h has mutation_hold_over_budget_total counter
  - evaluator_primitives_obs_eval.cpp query:mutation-boundary-hold-stats
    has schema-2313 / issue-2313 / mutation-hold-over-budget-total /
    mutation-hold-budget-us / mutation-hold-over-budget-wired keys
  - signal-only — does NOT force-fail or yield (would violate #2200 /
    unlock workspace_mtx_ mid-mutate)
  - tests/compiler/test_mutation_boundary_batch.cpp cites Issue #2313

Exit 0 on success, 1 on any failure. Run as part of the ship loop:
    python3 scripts/check_mutation_hold_over_budget_coverage.py
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
        # evaluator_mutation_boundary.cpp dtor + accessor
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "mutation_hold_budget_us",
            "dtor uses mutation_hold_budget_us() accessor",
        ),
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "mutation_hold_over_budget_total",
            "dtor bumps mutation_hold_over_budget_total",
        ),
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "AURA_MUTATION_HOLD_BUDGET_US",
            "env var AURA_MUTATION_HOLD_BUDGET_US documented",
        ),
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "Issue #2313",
            "evaluator_mutation_boundary.cpp cites 2313",
        ),
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "SIGNAL-ONLY",
            "dtor comment notes signal-only (no force-fail / yield)",
        ),
        # observability_metrics.h counter
        (
            ROOT / "src/compiler/observability_metrics.h",
            "mutation_hold_over_budget_total",
            "observability_metrics.h has counter",
        ),
        (ROOT / "src/compiler/observability_metrics.h", "// #2313", "observability_metrics.h cites 2313"),
        # evaluator_primitives_obs_eval.cpp query primitive
        (ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp", "schema-2313", "query primitive schema-2313"),
        (ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp", "issue-2313", "query primitive issue-2313"),
        (
            ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp",
            "mutation-hold-over-budget-total",
            "query primitive suppress counter key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp",
            "mutation-hold-budget-us",
            "query primitive budget-us key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp",
            "mutation-hold-over-budget-wired",
            "query primitive gate-wired sentinel",
        ),
        # fiber.h last_hold_us integration (#2253 closed loop)
        (ROOT / "src/serve/fiber.h", "set_last_hold_us", "fiber.h has set_last_hold_us (#2253 integration)"),
        (ROOT / "src/serve/fiber.h", "last_hold_us_", "fiber.h has last_hold_us_ field (#2253)"),
        # test file
        (ROOT / "tests/compiler/test_mutation_boundary_batch.cpp", "Issue #2313", "test file cites 2313"),
        (
            ROOT / "tests/compiler/test_mutation_boundary_batch.cpp",
            "run_2313_hold_budget",
            "test file has #2313 run function",
        ),
        # Linter self-reference (sanity)
        (
            ROOT / "scripts/check_mutation_hold_over_budget_coverage.py",
            "hold-budget over-budget cooperative safepoint",
            "linter self-reference",
        ),
    ]

    failed = 0
    for file, needle, label in checks:
        if not must_contain(file, needle, label):
            failed += 1

    if failed == 0:
        print(f"\nAll {len(checks)} #2313 checks passed.")
        return 0
    print(f"\n{failed} of {len(checks)} check(s) failed.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
