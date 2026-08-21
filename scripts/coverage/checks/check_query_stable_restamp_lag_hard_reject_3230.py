#!/usr/bin/env python3
"""Issue #3230: production query:*-stable hard-rejects restamp-lag before stamp-green.

After over-budget outermost restamp (lazy-align only), stamp_query_stable_ref_export
/ query:*-stable / as-stable-ref / ensure-ref must consult restamp_over_budget_torn
*before* make_ref_layout / make_safe_ref_layout and return structured restamp-lag
(budget-exceeded). Never a green StableNodeRef carrying a pre-mutate gen.
Soft observe; budget=0 quiet. No new public query key.

Contract:
  AC1 Production + over-budget → structured restamp-lag, never green ref
  AC2 Soft/Off / budget=0 observe-only
  AC3 Under-budget restamp_all unchanged
  AC4 Stamp path consults torn before make_ref_layout / export
  AC5 No new query key; reuse restamp-lag / budget-exceeded
  AC6 Extend hygiene + provenance_batch + query-result; linter; no invent

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _window(hay: str, needle: str, n: int = 900) -> str:
    i = hay.find(needle)
    if i < 0:
        return ""
    return hay[i : i + n]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    restamp = _read("src/core/flatast_restamp.hh")
    astx = _read("src/core/ast.ixx")
    sec = _read("src/compiler/evaluator_security.cpp")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    t = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    batch = _read("tests/compiler/test_stable_ref_provenance_batch.cpp")
    qrp = _read("tests/compiler/test_query_result_full_provenance.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_query.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )

    must("kQueryStableRestampLagHardRejectIssue = 3230", "AC1 stamp", restamp)
    must("restamp_over_budget_torn", "AC1 helper", restamp)
    must("ac3230_1_production_stamp_before_layout", "AC1 test", t)

    must("ac3230_2_soft_and_quiet", "AC2 test", t)
    must("should_hard_reject_soft_sibling", "AC2 Soft gate", sec)

    must("ac3230_3_under_budget_green", "AC3 test", t)
    must("!ws->restamp_over_budget_torn()", "AC3 quiet", t)

    stamp = _window(sec, "void Evaluator::stamp_query_stable_ref_export")
    if "allow_query_stable_ref_export" not in stamp:
        fails.append("AC4: stamp missing allow_query_stable_ref_export")
    if "Issue #3230" not in stamp:
        fails.append("AC4: stamp missing #3230 torn-before-layout cite")
    if "if (gen != 0)" in stamp and "ref.gen = gen" in stamp:
        fails.append("AC4: stamp still restores pre-mutate gen after make_ref_layout")
    must("restamp_over_budget_torn", "AC4 allow consults torn", sec)
    ens = qws
    idx = ens.find("held = ev.make_stamped_safe_ref")
    if idx < 0:
        fails.append("AC4: ensure-ref make_stamped_safe_ref missing")
    else:
        pre = ens[max(0, idx - 500) : idx]
        if "allow_query_stable_ref_export" not in pre:
            fails.append("AC4: ensure-ref make_stamped_safe_ref not preceded by allow")

    must("kRestampLagErrorKind", "AC5 reuse error", restamp)
    must("kRestampLagReasonBudgetExceeded", "AC5 reuse reason", restamp)
    if "schema-3230" in q:
        fails.append("AC5: new schema-3230 query key")
    if "g_3230_" in restamp:
        fails.append("AC5: new g_3230_* counter")

    must("check_query_stable_restamp_lag_hard_reject_3230", "AC6 build.py", build)
    must("3230", "AC6 provenance batch", batch)
    must("3230", "AC6 query-result", qrp)
    must("kQueryStableRestampLagHardRejectIssue", "AC6 ast export", astx)
    if (ROOT / "tests" / "issues" / "test_issue_3230.cpp").is_file():
        fails.append("AC6: forbidden tests/issues/test_issue_3230.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3230.cpp").is_file():
        fails.append("AC6: forbidden tests/compiler/test_issue_3230.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3230-*")):
            fails.append(f"AC6: docs/design/{f.name}")

    if fails:
        print("FAIL #3230 query_stable_restamp_lag_hard_reject:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3230 query_stable_restamp_lag_hard_reject: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
