#!/usr/bin/env python3
"""Issue #3318: coercion blame provenance restamp on mutation-epoch advance.

After nested mutate + epoch advance, residual CoercionEntry can keep a
stale source_mutation_id that no longer points at the live narrowing
predicate. Restamp from the active session mid/pred (never invent zero).
Production require-complete treats unrepaired stale as incomplete.
Reuses g_coercion_blame_epoch_restamp_total. No new query schema key.

Contract:
  AC1 After epoch advance, live non-zero mid >= current epoch mid (or elided)
  AC2 Soft observe restamp counter; Production stale is incomplete until restamp
  AC3 No change to identity elision / density policy
  AC4 extend test_coercion_stamp_at_add; this linter; no invent / docs

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

    cm = _read("src/compiler/coercion_map.ixx")
    pol = _read("src/compiler/coercion_provenance_policy.hh")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    tix = _read("src/compiler/type_checker.ixx")
    q = _read("src/compiler/evaluator_primitives_query.cpp") + _read(
        "src/compiler/evaluator_primitives_query_type_stats.cpp"
    )
    t = _read("tests/compiler/test_coercion_stamp_at_add.cpp")
    build = _read("build.py")

    must("kCoercionBlameEpochRestampIssue = 3318", "AC1 stamp", cm)
    must("current_mutation_epoch_mid", "AC1 floor", cm)
    must("restamp_coercion_entry_epoch_blame", "AC1 helper", cm)
    must("restamp_coercion_entry_epoch_blame", "AC1 apply", cm)
    must("restamp_coercion_epoch_blame", "AC1 persist", mb)
    must("restamp_epoch_blame", "AC1 TypeChecker map", tix)
    must("ac3318_1_epoch_advance_restamp", "AC1 test", t)

    must("coercion_entry_epoch_stale", "AC2 stale", cm)
    must("coercion_entry_dual_complete", "AC2 dual", cm)
    must("g_coercion_blame_epoch_restamp_total", "AC2 counter", cm)
    must("Issue #3318", "AC2 policy", pol)
    must("ac3318_2_soft_observe_production_incomplete", "AC2 test", t)

    must("identity coercion", "AC3 elision", cm)
    must("g_dead_coercion_ast_elided_total", "AC3 ast elide", cm)
    must("ac3318_3_elision_unchanged", "AC3 test", t)

    must("check_coercion_blame_epoch_restamp_3318", "AC4 build.py", build)
    must("ac3318_4_no_new_schema", "AC4 test", t)
    must("coercion-blame-epoch-restamp-total", "AC4 reuse key", q)
    if "schema-3318" in q:
        fails.append("AC4: new schema-3318 query key")
    if "g_3318_" in cm or "g_3318_" in mb:
        fails.append("AC4: new g_3318_* counter")
    if (ROOT / "tests" / "compiler" / "test_issue_3318.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3318.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3318.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3318.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3318-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3318 coercion_blame_epoch_restamp:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3318 coercion_blame_epoch_restamp: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
