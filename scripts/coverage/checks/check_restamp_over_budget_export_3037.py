#!/usr/bin/env python3
"""Issue #3037: restamp over-budget must reject StableNodeRef export.

Over-budget outermost restamp marks generation torn. Production
query:*-stable rejects (never a pre-mutate generation). Soft observe
only. Lazy-align of node_gen_ must not hide lag (eager bit).
Happy under-budget path is identical restamp_all_node_generations.

Contract (one row per AC):
  AC1 over-budget + production → query:*-stable error/empty/torn
      after lazy-align; never stale generation
  AC2 Soft only metric
  AC3 under-budget path zero regression
  AC4 additive counter + schema-3037 on stable-ref-stats
  AC5 extend #2960 isolation tests + this linter

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

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
    impl = _read("src/core/ast_impl.cpp")
    prov = _read("src/core/provenance_tracker.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    q = read_query_prims()
    qfile = _read("src/compiler/evaluator_primitives_query.cpp")
    gen = _read("src/compiler/evaluator_primitives_stdlib_review.cpp")
    qmid = _read("src/compiler/evaluator_primitives_query_obs_mid.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    test = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    cap = _read("tests/core/test_stable_ref_tenant_capture.cpp")
    build = _read("build.py")

    must("Issue #3037", "AC1 restamp", restamp)
    must("kRestampOverBudgetExportIssue = 3037", "AC1", restamp)
    must("restamp_generation_torn", "AC1", astx)
    must("node_eagerly_restamped", "AC1", astx)
    must("restamp_generation_torn_.store(1", "AC1 mark torn", impl)
    must("restamp_eager_", "AC1 eager bit", impl)
    must("node_eagerly_restamped", "AC1 allow", sec)
    must("record_query_stable_ref_restamp_torn_reject", "AC1", sec)
    must("production_defaults_active()", "AC1", sec)
    must("allow_query_stable_ref_export", "AC1 query", qws)
    must("generation torn", "AC1 torn message", qws)
    must("ac3037_1_production_torn_after_lazy_align", "AC1 test", test)
    must("reject_stable_ref_export", "AC1 helper comment", ev)

    must("ac3037_2_soft_observe_only", "AC2", test)
    must("record_query_stable_ref_restamp_torn_soft_observe", "AC2", sec)
    must("restamp_torn_soft_observe", "AC2", prov)

    must("ac3037_3_under_budget_zero_regression", "AC3", test)
    must("restamp_all_node_generations", "AC3 happy", impl)
    must("!ws->restamp_last_budget_exceeded()", "AC3 quiet", sec)

    must("query_stable_ref_restamp_torn_reject_total", "AC4", prov)
    must("kQueryStableRefRestampTornIssue = 3037", "AC4", prov)
    must("schema-3037", "AC4 stats-hash", qfile)
    must("query-stable-ref-restamp-torn-reject-total", "AC4 stats-hash", q)
    must("query-stable-ref-stamped-total", "AC4 #2960 preserved", q)
    must("schema-3000", "AC4 #3000 lineage", q)
    must("schema-3037", "AC4 generation-stats", gen)
    must("schema-3037", "AC4 children-stable-stats", qmid)
    must("ac3037_4_schema", "AC4 test", test)
    must("query:stable-ref-stats", "AC4 stable-ref-stats surface", qfile)

    must("ac3037_5_linter_and_suites", "AC5", test)
    must("#3037", "AC5 isolation", iso)
    must("#2960", "AC5 isolation lineage", iso)
    must("#3037", "AC5 capture", cap)
    must("check_restamp_over_budget_export_3037", "AC5", build)
    must("Issue #3037", "AC5 emb", emb)
    if (ROOT / "tests" / "compiler" / "test_issue_3037.cpp").is_file():
        fails.append("AC5: test_issue_3037.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3037.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3037.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3037-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3037 restamp over-budget export reject — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
