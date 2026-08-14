#!/usr/bin/env python3
"""Issue #3000: query:*-stable must not export a pre-mutate generation
when last restamp-budget exceeded and the node was not eagerly restamped.

Residual of #2934 (exit restamp budget) + #2960 (query:*-stable stamp).
#2934 AC2 keeps is_valid/make_ref consistent via lazy-align; this issue
closes the export face so Agents cannot cache a stamped-green pre-mutate
StableNodeRef.

Contract (one row per AC):
  AC1 Production + exceeded + node not restamped → query:*-stable rejects
      (typed restamp-lag) or does not return pre-mutate gen. Soft: stamp
      as today + observe.
  AC2 Unlimited / not exceeded → identical to #2960 (no new atomics).
  AC3 Never silent torn generation; reject is structured (restamp-lag),
      not -1 with no last-reason.
  AC4 Additive counters + schema-3000 on stable-ref-stats / generation-stats;
      stamped / unstamped_prevented non-regressing.
  AC5 Extend test_hygiene_mutate_closed_loop + isolation/tenant-capture
      (#81967). No test_issue_3000.cpp.
  AC6 Source-cite + this linter; no docs/design/* per #1655.

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
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    astx = _read("src/core/ast.ixx")
    restamp = _read("src/core/flatast_restamp.hh")
    prov = _read("src/core/provenance_tracker.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    ev = _read("src/compiler/evaluator.ixx")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    q = read_query_prims()
    gen = _read("src/compiler/evaluator_primitives_stdlib_review.cpp")
    qmid = _read("src/compiler/evaluator_primitives_query_obs_mid.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    test = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    iso = _read("tests/core/test_tenant_isolation_enforcement.cpp")
    cap = _read("tests/core/test_stable_ref_tenant_capture.cpp")
    build = _read("build.py")

    # AC1 — production reject / post-mutate gate before lazy-align.
    must("Issue #3000", "AC1", sec)
    must("allow_query_stable_ref_export", "AC1", sec)
    must("allow_query_stable_ref_export", "AC1", ev)
    must("node_generation_is_post_mutate", "AC1", astx)
    must("restamp_last_budget_exceeded", "AC1", sec)
    must("production_defaults_active()", "AC1", sec)
    must("allow_query_stable_ref_export", "AC1 children-stable", qws)
    must("allow_query_stable_ref_export", "AC1 parent-stable", qws)
    must("query:stable-ref", "AC1 stable-ref prim", qws)
    must("before make_ref_layout", "AC1 peek before lazy-align", ev)
    must("Issue #3000", "AC1 fiber batch", fiber)

    # AC2 — Soft observe; quiet happy path.
    must("restamp_lag_soft_observe", "AC2", sec)
    must("restamp_lag_soft_observe", "AC2", prov)
    must("one relaxed load", "AC2 quiet path", sec)
    must("not exceeded", "AC2", ev)

    # AC3 — structured reject, not silent -1.
    must("restamp-lag", "AC3", qws)
    must("restamp_lag_last_reason", "AC3", prov)
    must("silent torn generation", "AC3 preserve #2934 AC2", restamp)

    # AC4 — additive schema; #2960 counters kept.
    must("query_stable_ref_restamp_lag_prevented_total", "AC4", prov)
    must("kQueryStableRefRestampLagIssue = 3000", "AC4", prov)
    must("schema-3000", "AC4 stats-hash", q)
    must("query-stable-ref-restamp-lag-prevented-total", "AC4 stats-hash", q)
    must("query-stable-ref-stamped-total", "AC4 non-regress stamped", q)
    must("query-stable-ref-unstamped-prevented-total", "AC4 non-regress unstamped", q)
    must("schema-3000", "AC4 generation-stats", gen)
    must("schema-3000", "AC4 children-stable-stats", qmid)
    must("schema-2960", "AC4 keep 2960 lineage", q)
    must("schema-2934", "AC4 keep 2934 lineage", gen)

    # AC5 — extend existing suites; no invent test file.
    must("ac3000_1_production_reject_or_post_mutate", "AC5 hygiene", test)
    must("#3000", "AC5 hygiene", test)
    must("#3000", "AC5 isolation", iso)
    must("allow_query_stable_ref_export", "AC5 isolation", iso)
    must("#3000", "AC5 capture", cap)
    must("stamp_query_stable_ref_export", "AC5 capture", cap)
    if (ROOT / "tests" / "compiler" / "test_issue_3000.cpp").is_file():
        fails.append("AC5: test_issue_3000.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "core" / "test_issue_3000.cpp").is_file():
        fails.append("AC5: tests/core/test_issue_3000.cpp present (forbidden #81967)")

    # AC6 — source-cite + linter; no docs/design.
    must("Issue #3000", "AC6 emb", emb)
    must("Issue #3000", "AC6 restamp", restamp)
    must("Issue #3000", "AC6 ast", astx)
    must("check_query_stable_ref_restamp_lag_3000", "AC6", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("*3000*"):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3000 query stable-ref restamp-lag export face — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
