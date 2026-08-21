#!/usr/bin/env python3
"""Issue #3198: restamp budget exceed fail-closed on every Agent query export.

Production query:*-stable / ensure-ref / as-stable-ref already structured
restamp-lag (#3121). Residual: :as-query-result hashed QueryResult and
export_ref / export_ref_safe / export_held_ref could still emit a
layout-stamped handle after budget exceed. Unify on
allow_query_stable_ref_export. Soft observe-only. No new public query key.

Contract:
  AC1 Production + budget exceed + node not eagerly restamped → reject
      children-stable / parent-stable / stable-ref / ensure-ref /
      as-stable-ref / :as-query-result / export_ref* (not a durable pair/hash)
  AC2 Soft/Off observe-only; in-process make_stamped_ref still captures
  AC3 Outermost triad clears torn/budget; under-budget path unchanged
  AC4 Reuse restamp-lag / torn counters; no g_3198_* / public query key
  AC5 Extend hygiene + provenance_batch + query-result full provenance;
      no test_issue_3198.cpp / docs/design/

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _window(hay: str, needle: str, n: int = 1400) -> str:
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
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    asr = _read("src/compiler/evaluator_primitives_mutate.cpp")
    sec = _read("src/compiler/evaluator_security.cpp")
    t = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    batch = _read("tests/compiler/test_stable_ref_provenance_batch.cpp")
    qrp = _read("tests/compiler/test_query_result_full_provenance.cpp")
    build = _read("build.py")

    # AC1 stamp + uniform gate
    must("kQueryStableRestampExportUniformIssue = 3198", "AC1 stamp", restamp)
    must("kQueryStableRestampExportUniformIssue", "AC1 ast export", astx)
    must("allow_query_stable_ref_export", "AC1 children-stable", qws)
    must("allow_query_stable_ref_export", "AC1 parent-stable", qws)
    must("allow_query_stable_ref_export", "AC1 stable-ref", qws)
    must("allow_query_stable_ref_export", "AC1 ensure-ref", qws)
    must("allow_query_stable_ref_export", "AC1 as-stable-ref", asr)
    must('mev("restamp-lag"', "AC1 as-stable-ref structured", asr)
    must("budget-exceeded: :as-query-result:", "AC1 :as-query-result", qws)
    must("Issue #3198", "AC1 query sites", qws)
    must("Issue #3198", "AC1 export_ref", sec)
    must("stamp_ok", "AC1 children-stable nulled-ref", qws)

    exp = _window(sec, "Evaluator::export_ref(")
    if "allow_query_stable_ref_export" not in exp:
        fails.append("AC1: export_ref missing allow_query_stable_ref_export")
    exp_s = _window(sec, "Evaluator::export_ref_safe(")
    if "allow_query_stable_ref_export" not in exp_s:
        fails.append("AC1: export_ref_safe missing allow_query_stable_ref_export")
    held = _window(sec, "Evaluator::export_held_ref(")
    if "allow_query_stable_ref_export" not in held:
        fails.append("AC1: export_held_ref missing allow_query_stable_ref_export")

    asr_win = _window(asr, "query:as-stable-ref")
    if "allow_query_stable_ref_export" in asr_win and 'mev("restamp-lag"' not in asr_win:
        fails.append("AC1: query:as-stable-ref reject is not structured restamp-lag")

    hash_win = _window(qws, "auto make_query_result_hash", 9000)
    if "allow_query_stable_ref_export" not in hash_win:
        fails.append("AC1: make_query_result_hash missing allow_query_stable_ref_export")
    if "budget-exceeded: :as-query-result:" not in hash_win:
        fails.append("AC1: make_query_result_hash missing :as-query-result restamp-lag")

    must("ac3198_1_production_export_uniform", "AC1 test", t)
    must("ac3198_export_ref_fail_closed", "AC1 provenance batch", batch)

    # AC2
    must("ac3198_2_soft_shape_unchanged", "AC2 test", t)
    must("make_stamped_ref", "AC2 in-process", t)
    must("should_hard_reject_soft_sibling", "AC2 Soft gate", sec)

    # AC3
    must("ac3198_3_under_budget_green", "AC3 test", t)
    must("!ws->restamp_last_budget_exceeded()", "AC3 quiet allow", sec)

    # AC4 — no new public query key / invented counter
    if "g_3198_" in qws or "g_3198_" in sec or "g_3198_" in restamp:
        fails.append("AC4: invented g_3198_* counter")
    if "query:restamp-export-uniform" in qws or "query:3198" in qws:
        fails.append("AC4: new public query key")
    must("kRestampLagErrorKind", "AC4 reuse kind", restamp)
    must("kRestampLagReasonBudgetExceeded", "AC4 reuse reason", restamp)

    # AC5
    must("ac3198_4_source_and_linter", "AC5 test", t)
    must("check_query_stable_restamp_export_uniform_3198", "AC5 build.py", build)
    must("3198", "AC5 query-result provenance", qrp)
    must(
        "Issue #3198",
        "AC5 linter self",
        _read("scripts/coverage/checks/check_query_stable_restamp_export_uniform_3198.py"),
    )
    if (ROOT / "tests" / "compiler" / "test_issue_3198.cpp").is_file():
        fails.append("AC5: test_issue_3198.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3198.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3198.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3198-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3198 query-stable restamp export uniform — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
