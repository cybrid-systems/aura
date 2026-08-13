#!/usr/bin/env python3
"""Issue #2960: query:*-stable / children_stable full StableNodeRef provenance stamp.

Contract:
  AC1 Hot-path audit: FlatAST children/parent/for_each use make_ref_layout;
      query workspace stamps via stamp_query_stable_ref_export / make_stamped.
  AC2 Counters query_stable_ref_stamped_total / unstamped_prevented_total;
      schema-2960 on stable-ref-stats-hash + children-stable-stats.
  AC3 Multi-tenant isolation fail-closed (tests).
  AC4 Soft / single-tenant green; no docs/design/* (#1655).
  AC5 Extend existing suite (#81967); this linter; build gate.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    prov = _read("src/core/provenance_tracker.hh")
    ast = _read("src/core/ast.ixx")
    sec = _read("src/compiler/evaluator_security.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    qmid = _read("src/compiler/evaluator_primitives_query_obs_mid.cpp")
    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    test_cap = _read("tests/core/test_stable_ref_tenant_capture.cpp")
    build = _read("build.py")

    # AC1 — layout-only FlatAST + Evaluator stamp authority on query paths.
    must("Issue #2960", "AC1", ast)
    must("make_ref_layout", "AC1 parent/children", ast)
    # children_stable / for_each / parent_stable must not call Soft make_ref(cid).
    for name, body_pat in (
        (
            "children_stable",
            r"children_stable\s*\(NodeId id\)\s*const\s*\{(.+?)\n    \}",
        ),
        (
            "for_each_stable_child",
            r"for_each_stable_child\s*\(NodeId id,\s*Fn&&\s*fn\)\s*const\s*\{(.+?)\n    \}",
        ),
        (
            "parent_stable",
            r"parent_stable\s*\(NodeId id\)\s*const\s*noexcept\s*\{(.+?)\n    \}",
        ),
    ):
        m = re.search(body_pat, ast, re.MULTILINE | re.DOTALL)
        if not m:
            fails.append(f"AC1: {name} body not found")
            continue
        body = m.group(1)
        if "make_ref_layout" not in body:
            fails.append(f"AC1: {name} must use make_ref_layout")
        stripped = body.replace("make_ref_layout", "")
        if re.search(r"\bmake_ref\s*\(", stripped):
            fails.append(f"AC1: {name} must not Soft make_ref (use layout)")

    must("stamp_query_stable_ref_export", "AC1", ev)
    must("stamp_query_stable_ref_export", "AC1", sec)
    must("stamp_query_stable_ref_export", "AC1", qws)
    must("stamp_query_stable_ref_export", "AC1", fiber)
    # No brace-init StableNodeRef{id, gen} on query workspace returns.
    if re.search(r"StableNodeRef\s*\{[^}]*id[^}]*gen", qws):
        fails.append("AC1: query workspace must not brace-init StableNodeRef returns")

    # AC2 — counters + schema.
    must("query_stable_ref_stamped_total", "AC2", prov)
    must("query_stable_ref_unstamped_prevented_total", "AC2", prov)
    must("kQueryStableRefStampIssue", "AC2", prov)
    must("record_query_stable_ref_stamped", "AC2", prov)
    must("record_query_stable_ref_unstamped_prevented", "AC2", sec)
    must("query-stable-ref-stamped-total", "AC2", q)
    must("query-stable-ref-unstamped-prevented-total", "AC2", q)
    must("schema-2960", "AC2", q)
    must("issue-2960", "AC2", q)
    must("schema-2960", "AC2 children-stats", qmid)
    must("query-stable-ref-stamped-total", "AC2 children-stats", qmid)

    # AC3–AC5
    must("#2960", "AC5 test", test)
    must("stamp_query_stable_ref_export", "AC5 test", test)
    must("kQueryStableRefStampIssue", "AC5 test", test)
    must("#2960", "AC5 capture", test_cap)
    must("stamp_query_stable_ref_export", "AC5 capture", test_cap)
    must("check_query_stable_ref_stamp_2960", "AC5", build)
    if (ROOT / "tests" / "core" / "test_issue_2960.cpp").is_file():
        fails.append("AC5: test_issue_2960.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("*2960*"):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2960 query stable-ref full provenance stamp — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
