#!/usr/bin/env python3
"""
Linter for #2311 — fail-closed RenderFastExit suppress (linear / match-site
hard-gate). Closes the under-sample / soft-continue hole that
#2145/#2222/#2223 closed for Agent mutate but #2215 reopened for render
hotpath when the same Guard encloses linear ops or ADT match sites.

Verifies the implementation is wired correctly:
  - evaluator_mutation_boundary.cpp dtor computes linear_ops_present /
    match_sites_present / hard_gate via workspace walk + typed_audit
  - evaluator_mutation_boundary.cpp dtor bumps
    render_fast_exit_suppressed_linear_or_match_total +
    render_fast_exit_suppressed_linear_total +
    render_fast_exit_suppressed_match_total when suppress fires
  - observability_metrics.h has all three counters
  - evaluator_primitives_obs_eval.cpp query:mutation-boundary-hold-stats
    has schema-2311 / issue-2311 / suppress keys

Exit 0 on success, 1 on any failure. Run as part of the ship loop:
    python3 scripts/check_render_fast_exit_suppress_coverage.py
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
        # evaluator_mutation_boundary.cpp dtor suppress logic
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "render_fast_exit_suppressed_linear_or_match_total",
            "dtor bumps suppress counter",
        ),
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "linear_ops_present_local",
            "dtor computes linear_ops_present",
        ),
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "match_sites_present_local",
            "dtor computes match_sites_present",
        ),
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "hard_gate_local",
            "dtor computes hard_gate via typed_audit",
        ),
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "requires_invariant_hard_gate",
            "dtor calls typed_audit::requires_invariant_hard_gate",
        ),
        (ROOT / "src/compiler/evaluator_mutation_boundary.cpp", "Issue #2311", "dtor cites 2311"),
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "NodeTag::Linear",
            "linear detection via NodeTag::Linear",
        ),
        (ROOT / "src/compiler/evaluator_mutation_boundary.cpp", "NodeTag::Move", "linear detection via NodeTag::Move"),
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "render_fast_exit_suppressed_linear_total",
            "dtor bumps linear sub-counter",
        ),
        (
            ROOT / "src/compiler/evaluator_mutation_boundary.cpp",
            "render_fast_exit_suppressed_match_total",
            "dtor bumps match sub-counter",
        ),
        # observability_metrics.h counters
        (
            ROOT / "src/compiler/observability_metrics.h",
            "render_fast_exit_suppressed_linear_or_match_total",
            "observability_metrics.h has suppress-or-match counter",
        ),
        (
            ROOT / "src/compiler/observability_metrics.h",
            "render_fast_exit_suppressed_linear_total",
            "observability_metrics.h has linear counter",
        ),
        (
            ROOT / "src/compiler/observability_metrics.h",
            "render_fast_exit_suppressed_match_total",
            "observability_metrics.h has match counter",
        ),
        # query primitive keys
        (ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp", "schema-2311", "query primitive schema-2311"),
        (ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp", "issue-2311", "query primitive issue-2311"),
        (
            ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp",
            "render-fast-exit-suppressed-linear-or-match-total",
            "query primitive suppress-or-match key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp",
            "render-fast-exit-suppressed-linear-total",
            "query primitive linear key",
        ),
        (
            ROOT / "src/compiler/evaluator_primitives_obs_eval.cpp",
            "render-fast-exit-suppressed-match-total",
            "query primitive match key",
        ),
        # typed_audit API
        (
            ROOT / "src/compiler/typed_mutation_audit.h",
            "requires_invariant_hard_gate",
            "typed_audit::requires_invariant_hard_gate exists",
        ),
        # Issue #2626: unit test test_render_fast_exit_2215 removed with TUI stack.
        # Linter self-reference (sanity)
        (
            ROOT / "scripts/check_render_fast_exit_suppress_coverage.py",
            "fail-closed RenderFastExit suppress",
            "linter self-reference",
        ),
    ]

    failed = 0
    for file, needle, label in checks:
        if not must_contain(file, needle, label):
            failed += 1

    if failed == 0:
        print(f"\nAll {len(checks)} #2311 checks passed.")
        return 0
    print(f"\n{failed} of {len(checks)} check(s) failed.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
