#!/usr/bin/env python3
"""Issue #3426: held-cap overflow fail-closes the whole held set.

#3327 unions Agent-held ids into the over-budget hot cone, still capped
at kRestampHotConeHeldCap = 64. Excess was a silent prefix: first 64
eager-green, tail restamp-lag. Production overflow now skips held-prefix
eager and overflow dominates the eager-bit allow. Soft / unlatched:
zero extra. Reuse forced-stale / restamp-lag / torn. No new query key.

Contract:
  AC1 production + held 65 + over-budget → check_fresh false; allow false
  AC2 production + held ≤64 + node in cone still exportable (#3327)
  AC3 Soft / unlatched: zero extra
  AC4 #3327 union, #3386 torn probe OR, #3389 build-time cap non-regress
  AC5 tests in test_restamp_budget_hard_gate; linter after #3327;
      no docs/design/3426-*; no test_issue_3426.cpp

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
    sec = _read("src/compiler/evaluator_security.cpp")
    fiber = _read("src/compiler/evaluator_fiber_mutation.cpp")
    epoch = _read("src/core/workspace_epoch.hh")
    t = _read("tests/compiler/test_restamp_budget_hard_gate.cpp")
    build = _read("build.py")

    must("kRestampHotConeHeldOverflowIssue = 3426", "AC1 stamp", restamp)
    must("kRestampHotConeHeldCap = 64", "AC1 cap", restamp)
    must("restamp_hot_cone_held_overflow", "AC1 overflow helper", restamp)
    must("g_restamp_hot_cone_held_overflow", "AC1 overflow flag", restamp)
    must("Issue #3426", "AC1 impl skip prefix", impl)
    must("restamp_hot_cone_held_overflow", "AC1 skip consult", impl)
    must("Issue #3426", "AC1 allow dominate", sec)
    must("restamp_hot_cone_held_overflow", "AC1 allow helper", sec)
    must("test_3426_ac1_held_overflow_fail_closed", "AC1 test", t)

    must("test_3426_ac2_held_fits_still_exportable", "AC2 test", t)
    must("Issue #3327", "AC2 union cite", impl)
    must("restamp_hot_cone_held_count", "AC2 held walk", impl)

    must("should_hard_reject_soft_sibling()", "AC3 Soft gate", sec)
    must("test_3426_ac3_soft_zero_extra", "AC3 test", t)
    allow = sec[sec.find("Evaluator::allow_query_stable_ref_export") :]
    allow_win = allow[:1800] if allow else ""
    if "restamp_hot_cone_held_overflow" in allow_win and "should_hard_reject_soft_sibling()" not in allow_win:
        fails.append("AC3: overflow consult not behind Soft sibling gate")

    must("Issue #3327", "AC4 #3327 union", impl)
    must("restamp_over_budget_torn()", "AC4 #3386 probe", sec)
    must("kMaxInlineMatches = 64", "AC4 #3389 build cap", epoch)
    must("Issue #3426", "AC4 fiber cite", fiber)
    must("test_3426_ac4_non_regress", "AC4 test", t)

    must("check_restamp_hot_cone_held_overflow_3426", "AC5 build.py", build)
    prev = build.find("check_restamp_hot_cone_agent_held_3327")
    ours = build.find("check_restamp_hot_cone_held_overflow_3426")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: #3426 linter must run after #3327")
    if "g_3426_" in restamp or "g_3426_" in sec or "g_3426_" in impl:
        fails.append("AC5: new g_3426_* counter")
    if 'add("query:' in impl and "3426" in impl:
        fails.append("AC5: new public query key")
    if (ROOT / "tests" / "issues" / "test_issue_3426.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3426.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3426.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3426.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3426-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3426 restamp_hot_cone_held_overflow:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3426 restamp_hot_cone_held_overflow: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
