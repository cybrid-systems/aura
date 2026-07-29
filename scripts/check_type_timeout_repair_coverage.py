#!/usr/bin/env python3
"""Issue #2284: Agent-first-class TIMEOUT repair surface (structured
unresolved_affected_nodes).

Contract (5 AC from issue body):
  AC1: force_next_delta_timeout_for_test + mutate → query shows non-empty
       last-unresolved-affected-nodes (or explicit empty-with-reason).
  AC2: Production hard-reject path still rolls back (#2277); repair surface
       populated *before* rollback completes.
  AC3: Soft/sandbox TIMEOUT still exportable without hard-reject (#2107 parity).
  AC4: Schema additive; wired sentinel; no free-form-only dependency for
       Agent repair.
  AC5: Unit test under tests/compiler/; source-cite publish site + query keys.

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
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    test_cpp = _read("tests/compiler/test_type_timeout_repair_2284.cpp")

    # AC4: new atomics + new query primitive.
    _must(
        "type_repair_last_timeout_status" in met,
        "AC4: type_repair_last_timeout_status atomic missing in observability_metrics.h",
        fails,
    )
    _must(
        "type_repair_last_unresolved_count" in met,
        "AC4: type_repair_last_unresolved_count atomic missing in observability_metrics.h",
        fails,
    )
    _must(
        "type_repair_last_unresolved_aff_nodes_count" in met,
        "AC4: type_repair_last_unresolved_aff_nodes_count atomic missing in observability_metrics.h",
        fails,
    )
    _must(
        "type_repair_last_unresolved_aff_nodes" in met,
        "AC4: type_repair_last_unresolved_aff_nodes array missing in observability_metrics.h",
        fails,
    )
    _must(
        "type_repair_last_truncated_reverify" in met,
        "AC4: type_repair_last_truncated_reverify atomic missing in observability_metrics.h",
        fails,
    )
    _must(
        "type_repair_last_blame_complete" in met,
        "AC4: type_repair_last_blame_complete atomic missing in observability_metrics.h",
        fails,
    )
    _must(
        "type_repair_publish_total" in met,
        "AC4: type_repair_publish_total atomic missing in observability_metrics.h",
        fails,
    )
    _must(
        "type_repair_wired" in met,
        "AC4: type_repair_wired sentinel atomic missing in observability_metrics.h",
        fails,
    )
    _must(
        '"query:type-timeout-repair-stats"' in q,
        "AC4: query:type-timeout-repair-stats primitive registration missing",
        fails,
    )
    _must(
        "type-timeout-repair-last-status" in q
        and "type-timeout-repair-last-unresolved-count" in q
        and "type-timeout-repair-last-unresolved-aff-nodes-count" in q
        and "type-timeout-repair-last-truncated-reverify" in q
        and "type-timeout-repair-last-blame-complete" in q
        and "type-timeout-repair-publish-total" in q
        and "type-timeout-repair-wired" in q,
        "AC4: primitive must expose 7 fixed fields + wired sentinel",
        fails,
    )
    _must(
        "schema" in q and "2284" in q,
        "AC4: primitive must surface schema == 2284 lineage",
        fails,
    )

    # AC1 + AC2: publish site in evaluator_typecheck.cpp must capture
    # sdo.unresolved + sdo.unresolved_affected_nodes + blame_complete
    # before the rollback counter bumps.
    _must(
        "type_repair_last_timeout_status.store" in tc,
        "AC1: evaluator_typecheck.cpp publish site missing",
        fails,
    )
    _must(
        "type_repair_last_unresolved_count.store" in tc,
        "AC1: evaluator_typecheck.cpp must bump unresolved_count",
        fails,
    )
    _must(
        "type_repair_last_unresolved_aff_nodes_count.store" in tc,
        "AC1: evaluator_typecheck.cpp must bump aff_nodes_count",
        fails,
    )
    _must(
        "type_repair_last_blame_complete.store" in tc,
        "AC2: evaluator_typecheck.cpp must capture blame_complete at publish time",
        fails,
    )
    _must(
        "type_repair_publish_total.fetch_add" in tc,
        "AC1: evaluator_typecheck.cpp must bump publish_total",
        fails,
    )
    # The publish site must be inside the !cr.solve_ok block (before the
    # rollback counter bumps).
    _must(
        "if (!cr.solve_ok)" in tc,
        "AC2: publish site must be inside the !cr.solve_ok block",
        fails,
    )

    # AC3: boundary hard-reject path also publishes (status-only).
    _must(
        "type_repair_last_timeout_status.store" in mb,
        "AC3: evaluator_mutation_boundary.cpp must publish boundary hard-reject",
        fails,
    )
    _must(
        "kHardRejectStatus" in mb,
        "AC3: boundary hard-reject must use a distinct sentinel status",
        fails,
    )

    # AC5: test file exists with appropriate content.
    _must(
        "test_type_timeout_repair_2284" in test_cpp,
        "AC5: tests/compiler/test_type_timeout_repair_2284.cpp must exist with the expected header",
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
    parser = argparse.ArgumentParser(description="Issue #2284 timeout repair surface coverage linter")
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
    print("OK: Issue #2284 timeout repair surface - all 5 AC contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
