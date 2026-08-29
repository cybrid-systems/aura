#!/usr/bin/env python3
"""Issue #3349: DeadCoercion persist re-union before partial-relower impact_ub.

#3120 / #3228 remirror residual CastOp into type∪IR on type-txn / dirty-txn
/ commit_readiness. Residual: should_partial_relower_* / impact_ub consult
the dirty cone without a mandatory persist re-union, so a type-changed
CastOp can sit outside the partial peel cone.

Fix: force_residual_castop_undermark_into_cone before impact_ub in
relower_dirty_defines_from_workspace (and try_partial_invalidate_relower).
Mark persist-mapped IR blocks dirty; production persist + empty map →
fail-closed full. Reuses partial_forced_full_by_impact_total. Soft /
empty persist → 0 extra.

Contract:
  AC1 remirror precedes impact_ub; persist → block mark helper
  AC2 Soft / empty persist 0 extra; #3120/#3228/#3347 retained
  AC3 production persist remirrors; mark-or-full; no new query key
  AC4 after #3347; no invent / docs/design / g_3349_* / schema-3349

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

    svc = _read("src/compiler/service.ixx")
    dirty_cpp = _read("src/compiler/service_dirty.cpp")
    dirty = _read("src/compiler/dirty_propagation.ixx")
    t = _read("tests/compiler/test_dead_coercion_dirty_cone.cpp")
    build = _read("build.py")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp") + _read("src/compiler/evaluator_primitives_query.cpp")

    must("kDeadCoercionPersistBeforePartialIssue = 3349", "AC1 stamp", dirty)
    must("mark_entry_from_dead_coercion_persist_", "AC1 helper", svc)
    must("force_residual_castop_undermark_into_cone", "AC1 remirror", svc)
    must("ac3349_1_relower_remirrors_before_impact_ub", "AC1 test", t)

    pos = svc.find("if (want_partial && dirty_n > 0)")
    win = svc[pos : pos + 2800] if pos >= 0 else ""
    rem = win.find("force_residual_castop_undermark_into_cone")
    iub = win.find("impact_upper_bound_for_entry_")
    if pos < 0:
        fails.append("AC1: want_partial && dirty_n > 0 branch missing")
    elif rem < 0 or iub < 0 or rem > iub:
        fails.append("AC1: remirror must precede impact_ub in partial branch")
    must("force_residual_castop_undermark_into_cone", "AC1 try_partial", dirty_cpp)
    must("Issue #3349", "AC1 dirty cite", dirty_cpp)

    must("residual_castop_persist_active", "AC2 persist gate", dirty)
    must("ac3349_2_soft_quiet", "AC2 test", t)
    must("force_residual_castop_undermark_into_cone", "AC2 #3228 retained", dirty)
    must("kResidualCastopReadinessUndermarkIssue = 3347", "AC2 #3347 retained", dirty)

    must("mark_entry_from_dead_coercion_persist_", "AC3 mark path", svc)
    must("partial_forced_full_by_impact_total", "AC3 distinguisher", svc)
    must("ac3349_3_production_persist_marks_or_force_full", "AC3 test", t)
    if "schema-3349" in q:
        fails.append("AC3: new schema-3349 query key")
    if "g_3349_" in dirty or "g_3349_" in svc:
        fails.append("AC3: new g_3349_* counter")

    must("check_dead_coercion_persist_before_partial_3349", "AC4 build.py", build)
    must("check_residual_castop_readiness_undermark_3347", "AC4 after #3347", build)
    i3347 = build.find("check_residual_castop_readiness_undermark_3347")
    i3349 = build.find("check_dead_coercion_persist_before_partial_3349")
    if i3347 < 0 or i3349 < 0 or i3349 < i3347:
        fails.append("AC4: #3349 linter must run after #3347")
    must("ac3349_4_linter_no_invent", "AC4 test", t)
    if (ROOT / "tests" / "issues" / "test_issue_3349.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3349.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3349.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3349.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3349-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3349 dead_coercion_persist_before_partial:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3349 dead_coercion_persist_before_partial: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
