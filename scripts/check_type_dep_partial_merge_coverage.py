#!/usr/bin/env python3
"""Issue #2283: wire type_dep_graph_ into infer_flat_partial primary affected
set via touched_type_ids (CS touched_roots + occurrence vars + rebinding type
change).

Contract (5 AC from issue body):
  AC1: 50-binding body, rebind one symbol → type_dep path adds uses;
       visited count ≤ ancestor-only baseline.
  AC2: No under-invalidate: type change still reaches all live use-sites.
  AC3: Empty type_dep graph → zero overhead (no extra walks).
  AC4: Metrics + query keys additive on type-incremental fidelity / type-dep stats.
  AC5: Tests src-aligned; stress + incremental soundness sample still green.

This linter is the source-of-truth for the production surface.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = REPO / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8")


def _must(cond: bool, msg: str, fails: list) -> None:
    if not cond:
        fails.append(msg)


def check() -> list:
    fails = []

    met = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    tc_ixx = _read("src/compiler/type_checker.ixx")
    tc_impl = _read("src/compiler/type_checker_impl.cpp")
    test_cpp = _read("tests/compiler/test_type_dep_partial_merge_2283.cpp")

    # AC4: new metrics + new query primitive.
    _must(
        "type_dep_partial_merge_total" in met,
        "AC4: type_dep_partial_merge_total atomic missing in observability_metrics.h",
        fails,
    )
    _must(
        "type_dep_partial_nodes_added" in met,
        "AC4: type_dep_partial_nodes_added atomic missing in observability_metrics.h",
        fails,
    )
    _must(
        '"query:type-dep-partial-merge-stats"' in q,
        "AC4: query:type-dep-partial-merge-stats primitive registration missing",
        fails,
    )
    _must(
        "type-dep-partial-merge-total" in q
        and "type-dep-partial-nodes-added" in q
        and "type-dep-graph-affected-expand-total" in q,
        "AC4: primitive must expose merge-total + nodes-added + graph-affected-expand-total",
        fails,
    )

    # AC1 + AC2: new code block in infer_flat_partial walks touched_type_ids
    # from CS touched_roots + occurrence vars + rebinding type change, then
    # filters live_and_still_typed.
    _must(
        "touched_roots()" in tc_impl,
        "AC1: infer_flat_partial must call touched_roots() to seed touched_type_ids",
        fails,
    )
    _must(
        "type_dep_partial_merge_total" in tc_impl,
        "AC1: infer_flat_partial must bump type_dep_partial_merge_total",
        fails,
    )
    _must(
        "type_dep_partial_nodes_added" in tc_impl,
        "AC1: infer_flat_partial must bump type_dep_partial_nodes_added",
        fails,
    )
    _must(
        "static_cast<std::uint32_t>(flat.get(dep).type_id) != tid" in tc_impl,
        "AC2: live_and_still_typed filter must drop stale graph entries",
        fails,
    )

    # TypeChecker surface: touched_roots() getter must exist alongside
    # touched_roots_size().
    _must(
        "touched_roots_size()" in tc_ixx and "touched_roots()" in tc_ixx,
        "API: TypeChecker must expose both touched_roots_size() and touched_roots()",
        fails,
    )

    # AC3: empty graph → zero overhead. Counters only bump when
    # touched_type_ids is non-empty.
    _must(
        "if (!touched_type_ids.empty())" in tc_impl,
        "AC3: merge must skip when touched_type_ids is empty (zero overhead)",
        fails,
    )

    # AC5: test file exists with appropriate content.
    _must(
        "test_type_dep_partial_merge_2283" in test_cpp,
        "AC5: tests/compiler/test_type_dep_partial_merge_2283.cpp must exist with the expected header",
        fails,
    )
    _must(
        "AC1" in test_cpp and "AC2" in test_cpp and "AC3" in test_cpp and "AC4" in test_cpp and "AC5" in test_cpp,
        "AC5: test file must include all 5 AC sections",
        fails,
    )
    _must(
        "tests/compiler/" in test_cpp or "src/-aligned" in test_cpp.lower(),
        "AC5: test file must document src-aligned placement (tests/compiler/ per #81967)",
        fails,
    )

    return fails


def main() -> int:
    parser = argparse.ArgumentParser(description="Issue #2283 type_dep partial-merge coverage linter")
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run self-test (return 0 if contract satisfied)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Strict mode (non-zero exit on any failure)",
    )
    args = parser.parse_args()
    fails = check()
    if args.self_test:
        print(f"self-test: {len(fails)} failures")
        return 0 if not fails else 1
    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2283 type_dep partial-merge - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
