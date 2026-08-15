#!/usr/bin/env python3
"""Issue #3081: Soft allow_timeout_commit TIMEOUT is not query:type authority.

Soft + SolverBudget::allow_timeout_commit keeps TIMEOUT for observability
(#2900) but must clear type_export_authoritative so query:type /
query-type-of never surface a half-solved cone. Production fail-closed
(#2277 / #3003) is unchanged.

Contract (one row per AC):
  AC1 Soft + allow_timeout_commit + TIMEOUT → last_type_export_authoritative false
  AC2 query-type-of / get-inferred-type return not-authoritative
  AC3 Production/Full fail-closed counters unchanged
  AC4 SOLVED path does not write the flag (zero extra)
  AC5 Extend test_solve_delta_unresolved_export; this linter; no invent

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ixx = _read("src/compiler/type_checker.ixx")
    impl = _read("src/compiler/type_checker_impl.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    prim = _read("src/compiler/evaluator_primitives_eval.cpp")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    t = _read("tests/compiler/test_solve_delta_unresolved_export.cpp")
    build = _read("build.py")

    must("kSoftTimeoutExportNonAuthoritativeIssue = 3081", "AC1 stamp", ixx)
    must("Issue #3081", "AC1 infer_flat", impl)
    must("last_type_export_authoritative_ = false", "AC1 clear", impl)
    must("allow_timeout_commit", "AC1 Soft export", impl)
    must("ac3081_1_soft_timeout_clears_authority", "AC1 test", t)

    must("not-authoritative", "AC2 query", prim)
    must("query-type-of", "AC2 prim", prim)
    must("get-inferred-type", "AC2 prim", prim)
    must("Issue #3081", "AC2 typecheck-current", prim)
    must("grant_type_export_authority", "AC2 copy", prim)
    must("ac3081_2_query_type_not_authoritative", "AC2 test", t)

    must("delta_timeout_fail_closed_total", "AC3 #3003", impl)
    must("delta_timeout_reject_total", "AC3 #2277", impl)
    must("ac3081_3_production_unchanged", "AC3 test", t)

    must("ac3081_4_solved_zero_cost", "AC4 test", t)
    # Flag write is inside solve_status != SOLVED (export path only).
    if "if (solve_status != SolveResult::SOLVED)" not in impl:
        fails.append("AC4: infer_flat missing != SOLVED export branch")

    must("check_soft_timeout_export_non_authoritative_3081", "AC5 build.py", build)
    must("type_export_authoritative", "AC5 Evaluator", ev)
    must("Issue #3081", "AC5 typecheck mutate path", tc)
    must("ac3081_5_source_and_linter", "AC5 test", t)
    if (ROOT / "tests" / "compiler" / "test_issue_3081.cpp").is_file():
        fails.append("AC5: test_issue_3081.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3081-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3081 Soft TIMEOUT export non-authoritative — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
