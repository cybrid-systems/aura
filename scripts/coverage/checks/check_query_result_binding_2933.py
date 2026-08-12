#!/usr/bin/env python3
"""Issue #2933: first-class QueryResult object binding + SafePCVSpan pin.

Contract (one row per AC):
  AC1 QueryResult holds matches + QueryEpoch + optional pin; is_fresh
  AC2 Major query:* surfaces gain optional :as-query-result; default bare list
  AC3 QueryResult usable via query:result-fresh? / query:result-matches;
     stale under strict → query-epoch-stale
  AC4 Additive metrics query_result_created/fresh_hits/stale + schema-2933
  AC5 Soft default unchanged; no docs/design/*
  AC6 Source-cite in workspace_epoch.hh, query_workspace, query_matcher.ixx,
     extend test_query_epoch_contract (no invent test_issue_2933.cpp)

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hh = _read("src/core/workspace_epoch.hh")
    qw = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    qm = _read("src/compiler/query_matcher.ixx")
    test = _read("tests/compiler/test_query_epoch_contract.cpp")
    build = _read("build.py")

    # AC1
    must("struct QueryResult", "AC1", hh)
    must("QueryResultMatch", "AC1", hh)
    must("is_fresh", "AC1", hh)
    must("is_fresh_live", "AC1", hh)
    must("QueryEpoch", "AC1", hh)
    must("pinned", "AC1", hh)
    must("Issue #2933", "AC1", hh)

    # AC2
    must(":as-query-result", "AC2", qw)
    must(":query-result", "AC2", qw)
    must("make_query_result_hash", "AC2", qw)
    must("end_query_epoch_maybe_result", "AC2", qw)
    must("query:find", "AC2 find", qw)
    must("query:pattern", "AC2 pattern", qw)
    must("query:children-stable", "AC2 children-stable", qw)
    must("query:by-marker", "AC2 by-marker", qw)
    must("as_query_result", "AC2", qw)

    # AC3
    must("query:result-fresh?", "AC3", qw)
    must("query:result-matches", "AC3", qw)
    must("query-epoch-stale", "AC3", qw)

    # AC4
    must("g_query_result_created_total", "AC4", hh)
    must("g_query_result_fresh_hits_total", "AC4", hh)
    must("g_query_result_stale_total", "AC4", hh)
    must("query-result-created-total", "AC4", qw)
    must("query-result-fresh-hits-total", "AC4", qw)
    must("query-result-stale-total", "AC4", qw)
    must("schema-2933", "AC4", qw)
    must("schema-2192", "AC4 lineage", qw)

    # AC5 Soft default / no docs
    must("Default (no keyword) remains the bare list", "AC5", qw)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2933-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden)")

    # AC6 source + tests
    must("Issue #2933", "AC6 matcher", qm)
    must("QueryResult", "AC6 matcher", qm)
    must("Issue #2933", "AC6 test", test)
    must("QueryResult object binding", "AC6 test body", test)
    must("check_query_result_binding_2933", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2933.cpp").is_file():
        fails.append("AC6: invent test_issue_2933.cpp present")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("OK: Issue #2933 QueryResult binding — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
