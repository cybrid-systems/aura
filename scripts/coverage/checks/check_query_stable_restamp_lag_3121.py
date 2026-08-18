#!/usr/bin/env python3
"""Issue #3121: production query:*-stable restamp-lag is structured.

When restamp budget is exceeded and the node was not eagerly restamped,
production query:*-stable / as-stable-ref / ensure-ref must return
error="restamp-lag" + reason="budget-exceeded" (make_merr pair). Never a
green StableNodeRef or silent void. Soft observe-only.

Contract:
  AC1 Production + budget exceed + node not in eager cone → structured lag
  AC2 Soft/Off only bumps existing counters; return shape unchanged
  AC3 Happy under-budget path unchanged
  AC4 Source-cite flatast_restamp.hh + export sites; no docs/design/
  AC5 Extend test_hygiene_mutate_closed_loop; no test_issue_3121.cpp

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

    restamp = _read("src/core/flatast_restamp.hh")
    astx = _read("src/core/ast.ixx")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    asr = _read("src/compiler/evaluator_primitives_mutate.cpp")
    sec = _read("src/compiler/evaluator_security.cpp")
    t = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")

    must("Issue #3121", "AC1 restamp", restamp)
    must("kQueryStableRestampLagStructuredIssue = 3121", "AC1 stamp", restamp)
    must('kRestampLagErrorKind = "restamp-lag"', "AC1 error kind", restamp)
    must('kRestampLagReasonBudgetExceeded = "budget-exceeded"', "AC1 reason", restamp)
    must("budget-exceeded:", "AC1 children-stable", qws)
    must("budget-exceeded:", "AC1 parent-stable", qws)
    must("budget-exceeded:", "AC1 stable-ref", qws)
    must("budget-exceeded:", "AC1 ensure-ref", qws)
    must('mev("restamp-lag"', "AC1 as-stable-ref structured", asr)
    must("Issue #3121", "AC1 as-stable-ref cite", asr)
    must("ac3121_1_production_structured_lag", "AC1 test", t)
    asr_win = asr[asr.find("query:as-stable-ref") : asr.find("query:as-stable-ref") + 1200]
    if "allow_query_stable_ref_export" in asr_win and 'mev("restamp-lag"' not in asr_win:
        fails.append("AC1: query:as-stable-ref reject is not structured restamp-lag")

    must("ac3121_2_soft_shape_unchanged", "AC2 test", t)
    must("should_hard_reject_soft_sibling", "AC2 Soft gate", sec)

    must("ac3121_3_under_budget_green", "AC3 test", t)
    must("!ws->restamp_last_budget_exceeded()", "AC3 quiet allow", sec)

    must("kQueryStableRestampLagStructuredIssue", "AC4 ast export", astx)
    must("Issue #3121", "AC4 query sites", qws)
    must("Issue #3121", "AC4 restamp", restamp)
    must("ac3121_4_source_and_linter", "AC4 test", t)
    must("check_query_stable_restamp_lag_3121", "AC5 build.py", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3121.cpp").is_file():
        fails.append("AC5: test_issue_3121.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3121-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3121 query:*-stable structured restamp-lag — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
