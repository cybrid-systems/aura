#!/usr/bin/env python3
"""Issue #3839: plain grant_capability(string) session-binds high-risk under production.

Residual of #3561 — effect path set session_bound = production_defaults &&
is_high_risk, but the documented string path forced single_use only and
hard-coded session_bound=false (privilege-sticky after outermost Guard exit).
Option A aligns the string path with grant_effect_capability (no second model).

Contract (one row per AC):
  AC1  Evaluator::grant_capability(std::string) computes
       session_bound = production_defaults && is_high_risk and passes it to
       the 4-arg mirror (same #2882 high-risk mask); Soft/Off stays false.
  AC2  #3436 linter AC2 no longer documents intentional session_bound=false;
       fixture asserts session_bound + live residual under Restricted and
       Soft/Off no force; string "self-evo" aligns with #3721.
  AC3  Extends tests/core/test_capability_single_use_consume.cpp; linter +
       grandfather + build.py wired; no invent test_issue_3839.cpp /
       docs/design (#81967 / #1655). grant_effect_capability unchanged.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SEC = "src/compiler/evaluator_security.cpp"
TEST = "tests/core/test_capability_single_use_consume.cpp"
L3436 = "scripts/coverage/checks/check_grant_lifetime_alignment_3436.py"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
LINTER = "check_string_grant_session_bound_3839"


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r} present")

    sec = _read(SEC)
    test = _read(TEST)
    l3436 = _read(L3436)
    build = _read(BUILD)
    gf = _read(GF)

    # Span the single-arg string path only (before the 4-arg mirror).
    start = "void Evaluator::grant_capability(std::string cap) {"
    end = "void Evaluator::grant_capability(std::string cap, bool single_use"
    i = sec.find(start)
    j = sec.find(end, i + 1) if i >= 0 else -1
    s1 = sec[i:j] if i >= 0 and j >= 0 else ""
    if not s1:
        fails.append("AC1: string-path span markers not found")

    # AC1 — production high-risk session_bound on string path.
    must("Issue #3839", "AC1 cite", s1)
    must("kHighRiskMask", "AC1 mask", s1)
    must("is_high_risk", "AC1 is_high_risk", s1)
    must("session_bound = production_defaults && is_high_risk", "AC1 formula", s1)
    must("grant_capability(std::move(cap), single_use, session_bound,", "AC1 delegate", s1)
    must_not(
        "grant_capability(std::move(cap), single_use, /*session_bound=*/false,",
        "AC1 no hard-coded false",
        s1,
    )
    # Soft/Off: force only inside production_defaults && is_high_risk.
    must("production_defaults && is_high_risk", "AC1 Soft gate", s1)

    # grant_effect_capability formula retained (unchanged surface).
    must(
        "const bool session_bound = production_defaults && is_high_risk;",
        "AC1 effect path retained",
        sec,
    )

    # AC2 — #3436 linter + fixture.
    must("session_bound = production_defaults && is_high_risk", "AC2 3436 linter", l3436)
    must("Issue #3839", "AC2 3436 cites 3839", l3436)
    must_not(
        "grant_capability(std::move(cap), single_use, /*session_bound=*/false,",
        "AC2 3436 no false delegate",
        l3436,
    )
    must("3839 AC1", "AC2 fixture AC1", test)
    must("3839 AC2", "AC2 fixture AC2", test)
    must("3839 AC3", "AC2 fixture AC3", test)
    must("3839 AC4", "AC2 fixture AC4", test)
    must("ac3839_1_string_high_risk_session_revoked", "AC2 fixture fn", test)
    must("self-evo session_bound", "AC2 self-evo align", test)
    must(
        "AC1: string path session-binds high-risk under Restricted (#3839)",
        "AC2 3436 fixture updated",
        test,
    )
    must(
        "AC5: Off-mode string grant stays session_bound=false (#3839)",
        "AC2 Soft Off assert",
        test,
    )

    # AC3 — wiring + no invent.
    must(LINTER, "AC3 build registration", build)
    must("Issue #3839", "AC3 build cite", build)
    must("check_string_grant_session_bound_3839.py", "AC3 grandfather basename", gf)
    must(
        "scripts/coverage/checks/check_string_grant_session_bound_3839.py",
        "AC3 grandfather path",
        gf,
    )
    if (ROOT / "tests" / "core" / "test_issue_3839.cpp").is_file():
        fails.append("AC3: tests/core/test_issue_3839.cpp present (forbidden)")
    if (ROOT / "tests" / "compiler" / "test_issue_3839.cpp").is_file():
        fails.append("AC3: tests/compiler/test_issue_3839.cpp present (forbidden)")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        bad = [p.name for p in design.iterdir() if p.name.startswith("3839")]
        if bad:
            fails.append(f"AC3: docs/design/ has {bad}")

    if fails:
        print(f"check_string_grant_session_bound_3839: {len(fails)} row(s) failed:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3839 string grant_capability session-binds high-risk under production")
    return 0


if __name__ == "__main__":
    sys.exit(main())
