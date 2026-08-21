#!/usr/bin/env python3
"""Issue #3231: production QueryResult must force schema-2 full provenance.

:as-query-result finish under production_defaults_active must reject
layout-only schema-1 matches (error=query-result-layout-only) and bump
#3103 full-provenance-stale. Soft / no :as-query-result keeps schema-1.
No new public query key.

Contract:
  AC1 Production + :as-query-result → every match has_full_provenance
  AC2 Soft / no keyword: schema-1 still allowed
  AC3 is_fresh_with_refs production schema-2 fail-closed on mismatch
  AC4 push_match_full / finish gated on production_defaults_active
  AC5 No schema-3231; reuse #3103 counters
  AC6 Extend query_result_full_provenance + query_epoch_contract; linter

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

    hh = _read("src/core/workspace_epoch.hh")
    qw = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    t = _read("tests/compiler/test_query_result_full_provenance.cpp")
    ep = _read("tests/compiler/test_query_epoch_contract.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_query.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )

    must("kQueryResultLayoutOnlyRejectIssue = 3231", "AC1 stamp", hh)
    must("kQueryResultLayoutOnlyErrorKind", "AC1 error kind", hh)
    must("kQueryResultMatchSchema2", "AC1 schema-2 marker", hh)
    must("query-result-layout-only", "AC1 finish reject", qw)
    must("test_ac3231_production_as_query_result", "AC1 test", t)

    must("test_ac3231_schema2_marker_and_source", "AC2 source test", t)
    must("3231: Soft-default bare list", "AC2 epoch suite", ep)

    must("InvalidTenant", "AC3 tenant", hh)
    must("InvalidFiber", "AC3 fiber", hh)
    must("InvalidCowLayer", "AC3 cow", hh)
    must("InvalidMutation", "AC3 mutation", hh)
    must("production_defaults_active()", "AC3 production validator", qw)
    must("note_query_result_full_provenance_tenant_mismatch", "AC3 tenant counter", qw)

    must("production_defaults_active()", "AC4 finish gate", qw)
    must("push_match_full", "AC4 overload", hh)
    must("production_defaults_active()", "AC4 header cite", hh)

    must("note_query_result_full_provenance_stale", "AC5 reuse stale counter", qw)
    must("kQueryResultFullProvenanceIssue = 3103", "AC5 lineage", hh)
    if "schema-3231" in q or "schema-3231" in qw:
        fails.append("AC5: new schema-3231 query key")
    if "g_3231_" in hh:
        fails.append("AC5: new g_3231_* counter")

    must("check_query_result_layout_only_reject_3231", "AC6 build.py", build)
    must("3231", "AC6 epoch suite", ep)
    if (ROOT / "tests" / "issues" / "test_issue_3231.cpp").is_file():
        fails.append("AC6: forbidden tests/issues/test_issue_3231.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3231.cpp").is_file():
        fails.append("AC6: forbidden tests/compiler/test_issue_3231.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3231-*")):
            fails.append(f"AC6: docs/design/{f.name}")

    if fails:
        print("FAIL #3231 query_result_layout_only_reject:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3231 query_result_layout_only_reject: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
