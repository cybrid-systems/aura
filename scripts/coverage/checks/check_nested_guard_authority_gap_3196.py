#!/usr/bin/env python3
"""Issue #3196: nested Guard success authority-gap before outermost dtor.

Nested MutationBoundaryGuard success must publish a generation-torn /
authority-gap face that query:*-stable export / stamp / QueryResult
freshness consult. Outermost still owns unified_restamp triad. Soft:
zero extra.

Contract:
  AC1 Hard: production/Full nested success → defuse_index_ null + gap
      face; export fail-closed; no full unified_restamp on nested path
  AC2 Soft / Off: zero extra beyond today's nested path
  AC3 Outermost success unchanged; nested abort unchanged
  AC4 nested_authority_gap_total at CompilerMetrics end; no public query key
  AC5 Extend mutation_boundary_batch + hygiene nested Guard fixtures
  AC6 Source-cite linter + build.py; no docs/design / invent

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

    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    sec = _read("src/compiler/evaluator_security.cpp")
    ast = _read("src/core/ast.ixx")
    met = _read("src/compiler/observability_metrics.h")
    qw = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    hyg = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    batch = _read("tests/compiler/test_mutation_boundary_batch.cpp")
    build = _read("build.py")

    # AC1
    must("Issue #3196", "AC1 boundary", mb)
    must("note_nested_authority_gap", "AC1 note", mb)
    must("defuse_index_ = nullptr", "AC1 defuse", mb)
    must("nested_authority_gap()", "AC1 export", sec)
    must("note_nested_authority_gap", "AC1 FlatAST", ast)
    must("ac3196_1_production_nested_export_fail_closed", "AC1 test", hyg)
    must("run_3196_nested_authority_gap", "AC1 batch", batch)

    # Nested path must not call unified_restamp (outermost owns triad).
    nested_pos = mb.find("Issue #3196: nested success must publish")
    nested_win = mb[nested_pos : nested_pos + 650] if nested_pos >= 0 else ""
    if nested_pos >= 0 and "unified_restamp_after_boundary(" in nested_win:
        fails.append("AC1: nested #3196 block calls unified_restamp_after_boundary")

    # AC2
    must("Soft / Off: zero extra", "AC2 comment", mb)
    must("ac3196_2_soft_zero_extra", "AC2 test", hyg)

    # AC3
    must("clear_nested_authority_gap", "AC3 outermost clear", mb)
    must("unified_restamp_after_boundary", "AC3 outermost triad", mb)
    must("ac3196_3_outermost_clears_gap", "AC3 test", hyg)

    # AC4
    must("nested_authority_gap_total{0}", "AC4 counter", met)
    must("kNestedGuardAuthorityGapIssue = 3196", "AC4 stamp", met)
    if "nested-authority-gap-total" in qw or "nested-authority-gap-total" in sec:
        fails.append("AC4: public query key nested-authority-gap-total (forbidden)")
    if "g_3196_" in mb or "g_3196_" in met or "g_3196_" in ast:
        fails.append("AC4: new g_3196_* counter")

    # QueryResult freshness consults the face.
    must("nested_authority_gap()", "AC1 QueryResult", qw)

    # AC5 / AC6
    must("check_nested_guard_authority_gap_3196", "AC6 build.py", build)
    must("ac3196_4_source_and_linter", "AC5 hygiene source", hyg)

    if (ROOT / "tests" / "issues" / "test_issue_3196.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3196.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3196.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3196.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3196-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3196 nested_guard_authority_gap:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3196 nested_guard_authority_gap: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
