#!/usr/bin/env python3
"""Issue #3327: over-budget hot-cone includes Agent-held QueryResult / export set.

#3259 eager-restamps dirty roots + parent chain up to restamp_hot_cone_budget.
Cold Agent-held QueryResult / last-export StableNodeRef nodes outside that
cone still restamp-lag, forcing a full re-query. #3327 unions the held set
into the same cap. Soft / budget==0 never consults the held buffer.

Contract:
  AC1  held set that fits the cap is eagerly restamped (export succeeds)
  AC2  held set larger than cap → excess still restamp-lag (never green)
  AC3  Soft / budget==0 → no Agent-held walk
  AC4  extend tenant-capture + hygiene; reuse restamp-lag / torn counters
  AC5  linter after #3259; no docs/design/3327-*; no test_issue_3327.cpp

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
    impl = _read("src/core/ast_impl.cpp")
    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    sec = _read("src/compiler/evaluator_security.cpp")
    cap = _read("tests/core/test_stable_ref_tenant_capture.cpp")
    hyg = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")

    must("kRestampHotConeAgentHeldIssue = 3327", "AC1 stamp", restamp)
    must("note_restamp_hot_cone_held_node", "AC1 note", restamp)
    must("restamp_hot_cone_held_count", "AC1 cone union", impl)
    must("Issue #3327", "AC1 impl cite", impl)
    must("ac3327_1_held_set_export", "AC1 test", cap)

    must("ac3327_2_held_set_larger_than_cap", "AC2 test", cap)
    must("never green a pre-mutate gen", "AC2 fail-closed", impl)

    pos = impl.find("std::size_t FlatAST::restamp_hot_cone_after_budget")
    win = impl[pos : pos + 5000] if pos >= 0 else ""
    must("restamp_hot_cone_held_count", "AC3 held walk in impl", win)
    lu = fiber.find("if (r.budget_exceeded)")
    lu_win = fiber[lu : lu + 2800] if lu >= 0 else ""
    must("if (production)", "AC3 production gate", lu_win)
    must("restamp_hot_cone_after_budget", "AC3 hot-cone production-only", lu_win)
    must("ac3327_3_soft_no_held_walk", "AC3 test", cap)

    must("note_restamp_hot_cone_held_node", "AC4 last-export notes", sec)
    must("Issue #3327", "AC4 pin dump", fiber)
    must("ac3327_multi_round_held_cite", "AC4 hygiene", hyg)
    must("g_unified_restamp_torn_visible_total", "AC4 reuse torn", lu_win)

    must("check_restamp_hot_cone_agent_held_3327", "AC5 build.py", build)
    prev = build.find("check_restamp_hot_cone_budget_3259")
    ours = build.find("check_restamp_hot_cone_agent_held_3327")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3259")
    if "g_3327_" in restamp:
        fails.append("AC5: new g_3327_* counter")
    if (ROOT / "tests" / "issues" / "test_issue_3327.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3327.cpp per #81967")
    if (ROOT / "tests" / "core" / "test_issue_3327.cpp").is_file():
        fails.append("AC5: forbidden tests/core/test_issue_3327.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3327-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3327 restamp_hot_cone_agent_held:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3327 restamp_hot_cone_agent_held: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
