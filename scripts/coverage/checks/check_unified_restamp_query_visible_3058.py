#!/usr/bin/env python3
"""Issue #3058: unify restamp entry + query:*-stable over-budget torn visible.

Residual of #3019 / #3037:
  AC1  abort / steal / densify / boundary-success share
       unified_restamp_after_boundary; steal-adjacent probe no longer
       restamp_pinned-only
  AC2  over-budget → torn + query:as-stable-ref / query:ensure-ref /
       query:*-stable do not stamp-green a pre-restamp gen
  AC3  Soft / under-budget path unchanged
  AC4  additive schema-3058 only; no rename of restamp-lag / torn keys
  AC5  extend test_restamp_sla_observability (#81967); no test_issue_3058.cpp;
       no docs/design/ (#1655)

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
    ast = _read("src/core/ast.ixx")
    ev = _read("src/compiler/evaluator.ixx")
    fm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    asr = _read("src/compiler/evaluator_primitives_mutate.cpp")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    review = _read("src/compiler/evaluator_primitives_stdlib_review.cpp")
    q = _read("src/compiler/evaluator_primitives_query.cpp")
    test = _read("tests/core/test_restamp_sla_observability.cpp")
    build = _read("build.py")

    must("kUnifiedRestampQueryVisibleIssue = 3058", "AC1 stamp", restamp)
    must("kUnifiedRestampQueryVisibleIssue", "AC1 ast export", ast)
    must("Issue #3058", "AC1 evaluator", ev)
    must("unified_restamp_after_boundary", "AC1 helper", ev)
    must("UnifiedRestampSite::BoundarySuccess", "AC1 boundary", mb)
    must("UnifiedRestampSite::AbortRestore", "AC1 abort", mb)
    must("UnifiedRestampSite::StealComplete", "AC1 steal", fm)
    must("UnifiedRestampSite::Densify", "AC1 densify", fm)
    must("ac3058_1_unified_entry_no_steal_split", "AC1 test", test)
    probe = fm.find("void Evaluator::probe_and_repin_linear_on_steal")
    if probe < 0:
        fails.append("AC1: probe_and_repin_linear_on_steal missing")
    else:
        body = fm[probe : probe + 700]
        if "restamp_pinned_stable_refs()" in body:
            fails.append("AC1: steal probe still restamp_pinned-only")
        if "unified_restamp_after_boundary" not in body:
            fails.append("AC1: steal probe does not call unified")

    must("allow_query_stable_ref_export", "AC2 as-stable-ref", asr)
    must("Issue #3058", "AC2 as-stable-ref cite", asr)
    must("query:ensure-ref: restamp budget exceeded", "AC2 ensure-ref", qws)
    must("schema-3058", "AC2 ensure-ref schema", qws)
    must("ac3058_2_over_budget_query_stable_visible", "AC2 test", test)

    must("ac3058_3_soft_under_budget_unchanged", "AC3 test", test)
    must("skipped_extra = true", "AC3 Soft skip", fm)

    must("schema-3058", "AC4 review", review)
    must("query-stable-ref-over-budget-visible-wired", "AC4 wired", review)
    must("schema-3000", "AC4 lag preserved", review)
    must("schema-3037", "AC4 torn preserved", review)
    must("schema-3058", "AC4 stats-hash", q)
    must("ac3058_4_additive_schema", "AC4 test", test)

    must("ac3058_5_canary_linter", "AC5 test", test)
    must("check_unified_restamp_query_visible_3058", "AC5 build", build)
    if _read("tests/core/test_issue_3058.cpp"):
        fails.append("AC5: test_issue_3058.cpp present")
    if _read("docs/design/3058-unified-restamp-query.md"):
        fails.append("AC5: docs/design/3058-* present")

    if fails:
        print(f"Issue #3058 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3058 unified restamp query-visible — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
