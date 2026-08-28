#!/usr/bin/env python3
"""Issue #3259: production restamp budget exceed eager-restamps hot cone.

When outermost restamp_all degrades to lazy-align (over AURA_RESTAMP_BUDGET_NODES),
production must eager-restamp dirty roots + parent chain up to
restamp_hot_cone_budget (default budget/2) so Agent-held StableNodeRef /
QueryResult on those nodes stay exportable. Remainder stays torn /
restamp-lag. Soft / budget==0 / no production_defaults: zero extra.
Nested Guard success may thin-hot-cone (#3312) but never runs
unified_restamp_after_boundary (outermost still owns the triad).

Contract:
  AC1  production + over-budget + hot cone → export / query:*-stable succeed
  AC2  node outside hot cone → structured restamp-lag (never green pre-mutate)
  AC3  Soft / budget==0 / !production → zero extra
  AC4  restamp_over_budget_torn / torn-visible / query-epoch stale remain
  AC5  nested success does not run unified_restamp; linter after #3258; no invent

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
    astx = _read("src/core/ast.ixx")
    impl = _read("src/core/ast_impl.cpp")
    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    sec = _read("src/compiler/evaluator_security.cpp")
    cap = _read("tests/core/test_stable_ref_tenant_capture.cpp")
    t = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")
    l3000 = _read("scripts/coverage/checks/check_query_stable_ref_restamp_lag_3000.py")
    l3230 = _read("scripts/coverage/checks/check_query_stable_restamp_lag_hard_reject_3230.py")

    must("kRestampHotConeBudgetIssue = 3259", "AC1 issue", restamp)
    must("restamp_hot_cone_budget", "AC1 cap helper", restamp)
    must("AURA_RESTAMP_HOT_CONE_FRAC", "AC1 env", restamp)
    must("restamp_hot_cone_after_budget", "AC1 FlatAST", astx)
    must("restamp_hot_cone_after_budget", "AC1 impl", impl)
    must("ac3259_1_hot_cone_export", "AC1 tenant-capture", cap)
    must("ac3259_1_hot_cone_query_stable", "AC1 hygiene", t)

    lu = fiber.find("if (r.budget_exceeded)")
    lu_win = fiber[lu : lu + 2000] if lu >= 0 else ""
    must("restamp_hot_cone_after_budget", "AC1 unified", lu_win)
    must("Issue #3259", "AC1 unified cite", lu_win)
    must("if (production)", "AC1 production gate", lu_win)

    must("ac3259_2_outside_cone_restamp_lag", "AC2 tenant-capture", cap)
    must("ac3259_2_outside_cone_query_lag", "AC2 hygiene", t)
    must("never green a pre-mutate gen", "AC2 fail-closed", fiber)
    must("Issue #3259", "AC2 stamp/allow", sec)

    must("ac3259_3_soft_gen0_zero_extra", "AC3 tenant-capture", cap)
    must("ac3259_3_soft_zero_extra", "AC3 hygiene", t)
    must("if (budget == 0)", "AC3 cap zero", restamp)
    must("return 0", "AC3 cap no-op", restamp)

    must("g_unified_restamp_torn_visible_total", "AC4 torn-visible", lu_win)
    must("force_query_epoch_stale_from_restamp_budget", "AC4 query-epoch", lu_win)
    must("ac3259_4_torn_counters_accurate", "AC4 tenant-capture", cap)
    must("restamp_over_budget_torn", "AC4 helper kept", restamp)

    nest = emb.find("if (workspace_flat_ && !stack.empty())")
    nwin = emb[nest : nest + 3200] if nest >= 0 else ""
    must("restamp_all_node_generations", "AC5 nested restamp_all", nwin)
    if "unified_restamp_after_boundary(" in nwin:
        fails.append("AC5: nested must not call unified_restamp_after_boundary")
    must("Issue #3259", "AC5 nested cite", nwin)
    must("Issue #3312", "AC5 nested thin hot-cone", nwin)
    must("ac3259_5_source_and_linter", "AC5 tenant-capture", cap)
    must("check_restamp_hot_cone_budget_3259", "AC5 build.py", build)
    prev = build.find("check_abort_force_lookup_reject_3258")
    ours = build.find("check_restamp_hot_cone_budget_3259")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3258")
    must("3259", "AC5 extend 3000 linter", l3000)
    must("3259", "AC5 extend 3230 linter", l3230)

    if (ROOT / "tests" / "issues" / "test_issue_3259.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3259.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3259.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3259.cpp per #81967")
    if (ROOT / "tests" / "core" / "test_issue_3259.cpp").is_file():
        fails.append("AC5: forbidden tests/core/test_issue_3259.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3259-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if "schema-3259" in t or "schema-3259" in cap:
        fails.append("AC5: new schema-3259 query key (SlimSurface)")
    if "g_3259_" in restamp:
        fails.append("AC5: new g_3259_* counter")

    if fails:
        print("FAIL #3259 restamp_hot_cone_budget:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3259 restamp_hot_cone_budget: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
