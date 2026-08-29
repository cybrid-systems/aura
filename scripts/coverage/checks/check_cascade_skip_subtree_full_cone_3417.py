#!/usr/bin/env python3
"""Issue #3417: cascade_mark_dirty skip_subtree is 1-hop, not full cone.

Production/Full must not skip BFS expansion because immediate dependents
are dirty — grandchildren / new DepGraph edges would miss incremental
typecheck. Soft/Off keep the #2063 1-hop skip + dirty_skip_subtree.
Reuse cascade_skip_subtree_total / cascade_roots_dropped_no_dep_graph_total.
No new query key.

Contract:
  AC1 Production/Full drop 1-hop skip (or require current-cascade epoch)
  AC2 cascade(R) marks every DepGraph-reachable node
  AC3 Soft/Off keep 1-hop skip + dirty_skip_subtree observe
  AC4 no new query key; reuse existing skip / dropped-roots counters
  AC5 extend cascade + hot-pass suites; linter after #3347; no invent

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

    dirty = _read("src/compiler/dirty_propagation.ixx")
    tcas = _read("tests/compiler/test_dirty_propagation_cascade.cpp")
    tsoa = _read("tests/compiler/test_hot_pass_dirty_soa.cpp")
    tskip = _read("tests/compiler/test_cascade_skip_metrics.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_query.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )

    must("kCascadeSkipSubtreeFullConeIssue = 3417", "AC1 stamp", dirty)
    must("cascade_mark_dirty", "AC1 cascade", dirty)
    fn = dirty.find("inline std::size_t cascade_mark_dirty")
    fn_win = dirty[fn : fn + 4500] if fn >= 0 else ""
    must("production_defaults_active()", "AC1 production gate", fn_win)
    must("AuditStrategy::Full", "AC1 Full gate", fn_win)
    must("!hard && set.is_dirty(nxt)", "AC1 drop skip under hard", fn_win)

    must("q.push({nxt, depth + 1})", "AC2 enqueue reachable", fn_win)
    must("ac3417_production_full_cone", "AC2 cascade test", tcas)
    must("production cascade(R) marks grandchild C", "AC2 fixture", tcas)

    must("dirty_skip_subtree.fetch_add", "AC3 Soft skip observe", fn_win)
    must("Soft 1-hop skip may leave C clean", "AC3 Soft fixture", tcas)
    must("apply_dev_audit_defaults", "AC3 skip-metrics Soft", tskip)

    must("cascade_skip_subtree_total", "AC4 reuse skip total", dirty)
    must("cascade_roots_dropped_no_dep_graph_total", "AC4 reuse dropped", dirty)
    if "schema-3417" in q or "schema-3417" in dirty:
        fails.append("AC4: new schema-3417 query key (forbidden)")
    if "g_3417_" in dirty:
        fails.append("AC4: new g_3417_* counter (forbidden)")

    must("check_cascade_skip_subtree_full_cone_3417", "AC5 build.py", build)
    must("check_residual_castop_readiness_undermark_3347", "AC5 after #3347", build)
    i3347 = build.find("check_residual_castop_readiness_undermark_3347")
    i3417 = build.find("check_cascade_skip_subtree_full_cone_3417")
    if i3347 < 0 or i3417 < 0 or i3417 < i3347:
        fails.append("AC5: #3417 linter must run after #3347")
    must("3417", "AC5 hot-pass suite", tsoa)
    must("3417", "AC5 skip-metrics suite", tskip)
    if (ROOT / "tests" / "compiler" / "test_issue_3417.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3417.cpp present (forbidden)")
    if (ROOT / "tests" / "issues" / "test_issue_3417.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3417.cpp present (forbidden)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3417-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("check_cascade_skip_subtree_full_cone_3417: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3417 cascade skip_subtree full cone — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
