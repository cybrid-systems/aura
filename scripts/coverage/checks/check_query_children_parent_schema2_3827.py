#!/usr/bin/env python3
"""Issue #3827: query:children / query:parent Production export is schema-2.

Production hardens input resolve (#3395) and durable find via
end_query_epoch_maybe_result → schema-2. Pre-#3827, query:children /
query:parent still finished with plain end_query_epoch and returned bare
NodeId lists under Production — Agents caching those ints hit the
occupancy hole. Soft keeps historical bare lists.

Contract (one row per AC):
  AC1  query:children / query:parent finish via end_query_epoch_maybe_result
  AC2  Soft children ints fail as-stable-ref under Prod (test + #3395 gate)
  AC3  *-stable path unchanged (children-stable still maybe_result)
  AC4  Soft bare list preserved; stamp; suite home; no invent/docs

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

    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    hh = _read("src/core/workspace_epoch.hh")
    t = _read("tests/compiler/test_query_result_full_provenance.cpp")
    build = _read("build.py")

    must("Issue #3827", "AC1 cite", qws)
    must("kQueryChildrenParentSchema2ExportIssue = 3827", "AC1 stamp", hh)

    ch = qws.find('(*q_impls)["query:children"]')
    ch_end = qws.find('(*q_impls)["query:children-stable"]', ch)
    ch_body = qws[ch:ch_end] if ch >= 0 and ch_end > ch else ""
    must("end_query_epoch_maybe_result", "AC1 children finish", ch_body)
    must("Issue #3827", "AC1 children cite", ch_body)
    # Must not finish with plain end_query_epoch( only (allow maybe_result).
    if "return end_query_epoch(qe," in ch_body:
        fails.append("AC1: query:children still returns end_query_epoch( bare)")

    pa = qws.find('(*q_impls)["query:parent"]')
    pa_end = qws.find("query:siblings", pa)
    pa_body = qws[pa:pa_end] if pa >= 0 and pa_end > pa else (qws[pa : pa + 3500] if pa >= 0 else "")
    must("end_query_epoch_maybe_result", "AC1 parent finish", pa_body)
    must("Issue #3827", "AC1 parent cite", pa_body)
    if "return end_query_epoch(qe," in pa_body:
        fails.append("AC1: query:parent still returns end_query_epoch( bare)")

    must("test_ac3827_2_soft_children_int_fails_prod_as_stable", "AC2 test", t)
    must("Soft children int is stale-ref under Prod", "AC2 oracle", t)

    cs = qws.find('(*q_impls)["query:children-stable"]')
    cs_end = qws.find('(*q_impls)["query:parent-stable"]', cs)
    cs_body = qws[cs:cs_end] if cs >= 0 and cs_end > cs else ""
    must("end_query_epoch_maybe_result", "AC3 children-stable finish", cs_body)
    must("test_ac3827_3_children_stable_stays_green", "AC3 test", t)

    must("test_ac3827_1_production_children_v2_schema2", "AC4 AC1 test", t)
    must("test_ac3827_4_soft_and_source", "AC4 Soft test", t)
    must("Soft children is NOT a hash", "AC4 Soft oracle", t)
    must("check_query_children_parent_schema2_3827", "AC4 build.py", build)

    if (ROOT / "tests" / "compiler" / "test_issue_3827.cpp").is_file():
        fails.append("AC4: test_issue_3827.cpp present (forbidden invent)")
    if (ROOT / "tests" / "issues" / "test_issue_3827.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3827.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3827-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print(f"Issue #3827 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3827 query children/parent schema-2 — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
