#!/usr/bin/env python3
"""Issue #3428: view_at HARDEN-armed AURA_HOT_CHECK (I1 residual of #3106).

#3106 AC1 named view_at as a harden site. The macro landed; the call
site only AURA_HOT_RECORD()'d. Language pre is stripped under NDEBUG,
CHECK_CONSTEXPR is Enforce-only. HARDEN armed was still Quiet OOB.

Contract:
  AC1 HARDEN + OOB view_at → trap + abort; view_at body has AURA_HOT_CONTRACT
  AC2 production OFF / NDEBUG without HARDEN: no extra atomic beyond armed() load
  AC3 #3106 / #3043 suites stay green; do not weaken check_hot_contract_harden_3106
  AC4 extend test_hot_contract_placement; linter after #3106;
      no docs/design/3428-*; no test_issue_3428.cpp
  AC5 no new public query key; reuse hot-contract-harden-trap-total / armed;
      no Arena / Shape version policy change

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

    soa = _read("src/compiler/ir_soa.ixx")
    hh = _read("src/core/cpp26_contract_stats.h")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_hot_contract_placement.cpp")
    build = _read("build.py")
    lint3106 = _read("scripts/coverage/checks/check_hot_contract_harden_3106.py")

    start = soa.find("IRInstructionView view_at(")
    nxt = soa.find("add_block", start) if start >= 0 else -1
    win = soa[start:nxt] if start >= 0 and nxt > start else ""

    must("AURA_HOT_CONTRACT", "AC1 CONTRACT in view_at", win)
    must("pre(func_idx < functions.size())", "AC1 pre cold", win)
    must("func_idx < functions.size()", "AC1 func bound", win)
    must("idx < functions[func_idx].size()", "AC1 idx bound", win)
    must("Issue #3428", "AC1 cite", win)
    must("record_hotpath_contract_harden_trap", "AC1 trap helper", hh)
    must("std::abort()", "AC1 abort", hh)
    must("3428 AC1", "AC1 test", test)

    must("AURA_HOT_CONTRACT", "AC2 CONTRACT one-liner", win)
    must("hot_contract_harden_armed()", "AC2 OFF gate", hh)
    must("expr not evaluated", "AC2 Soft skip", hh)
    must("3428 AC2", "AC2 test", test)

    must("3106 AC1", "AC3 3106", test)
    must("3043 AC1", "AC3 3043", test)
    must("AURA_HOT_CHECK_CONSTEXPR", "AC3 constexpr kept", soa)
    must("check_hot_contract_harden_3106", "AC3 3106 linter still wired", build)
    must("3106", "AC3 3106 linter not deleted", lint3106)
    if "view_at body contains AURA_HOT_CHECK" in lint3106:
        fails.append("AC3: do not weaken check_hot_contract_harden_3106.py")

    must("check_hot_contract_view_at_harden_3428", "AC4 build.py", build)
    must("3428 AC4", "AC4 test", test)
    prev = build.find("check_hot_contract_harden_3106")
    ours = build.find("check_hot_contract_view_at_harden_3428")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: #3428 linter must run after #3106")
    if (ROOT / "tests" / "compiler" / "test_issue_3428.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3428.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3428.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3428.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3428-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    must("hot-contract-harden-trap-total", "AC5 reuse trap-total", q)
    must("hot-contract-harden-armed", "AC5 reuse armed", q)
    must("3428 AC5", "AC5 test", test)
    if "schema-3428" in q:
        fails.append("AC5: new schema-3428 query key")
    if "g_3428_" in soa or "g_3428_" in hh:
        fails.append("AC5: new g_3428_* counter")
    must("#define AURA_COLD_CONTRACT(expr) ((void)0)", "AC5 cold off unchanged", hh)

    if fails:
        print("FAIL #3428 hot_contract_view_at_harden:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3428 hot_contract_view_at_harden: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
