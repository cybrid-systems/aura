#!/usr/bin/env python3
"""Issue #3828: mutate:from-verification-feedback via add_mutate SSOT.

Pre-#3828 the prim registered via raw add() in evaluator_primitives_compile.cpp
(missed #3697 persist-reject / RO fence / naked belt / #3395 packed-ref).
Strategies may stay Soft dormant #f; registration must still be add_mutate.

Contract (one row per AC):
  AC1 Gate fails on raw add("mutate: outside exempt window in compile.cpp
  AC2 Live strategy under Production hits Guard metrics + persist-reject
      path + packed-ref resolve (add_mutate + resolve_mutate_node_arg)
  AC3 Soft dormant #f body may remain until strategies return
  AC4 Suite / stamp / build wiring; no invent test / no docs/design/

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

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    compile_cpp = _read("src/compiler/evaluator_primitives_compile.cpp")
    hh = _read("src/compiler/mutate_dispatch.hh")
    sg = _read("scripts/coverage/checks/check_mutate_dispatch_sole_guard_3074.py")
    se = _read("scripts/coverage/checks/check_mutate_dispatch_sole_entry_3192.py")
    t = _read("tests/compiler/test_mutate_batch.cpp")
    build = _read("build.py")

    # AC1 — compile.cpp has no raw add("mutate:
    for i, ln in enumerate(compile_cpp.splitlines(), 1):
        s = ln.lstrip()
        if s.startswith("//") or s.startswith("*"):
            continue
        if 'add("mutate:' in ln:
            fails.append(f'AC1: raw add("mutate: at compile.cpp:{i}')
    must("Issue #3828", "AC1 cite compile empty", compile_cpp)
    must("AC3452/#3828", "AC1 sole_guard scans compile", sg)
    must("AC2/#3828", "AC1 #3192 scans compile", se)

    # AC2 — add_mutate + resolve + persist-reject / packed-ref lineage
    pos = mut.find('"mutate:from-verification-feedback"')
    if pos < 0:
        fails.append("AC2: mutate:from-verification-feedback missing in mutate.cpp")
        body = ""
    else:
        look = mut[max(0, pos - 120) : pos + 4500]
        body = look
        if "add_mutate" not in mut[max(0, pos - 80) : pos]:
            fails.append("AC2: not registered through add_mutate")
    must("resolve_mutate_node_arg", "AC2 packed-ref resolve", body)
    must("a.subspan(1)", "AC2 node at a[1]", body)
    must("require_effect_for_node_id", "AC2 for_node_id", body)
    must("Issue #3395", "AC2 bare-int cite", body)
    must("Issue #3697", "AC2 persist-reject cite", mut)  # add_mutate wrapper
    must("kMutateFromVerificationFeedbackSsotIssue = 3828", "AC2 stamp", hh)
    must("FromVerificationFeedback", "AC2 MutateKind", hh)

    # AC3 — Soft dormant #f strategies retained
    must("eda:weaken-property retired", "AC3 dormant weaken", body)
    must("eda:add-coverpoint-bin retired", "AC3 dormant coverpoint", body)
    must("eda:update-constraint retired", "AC3 dormant constraint", body)
    must("Soft dormant", "AC3 soft cite", body)
    must("test_ac3828_", "AC3 suite", t)

    # AC4 — wiring / no invent
    must("check_mutate_from_feedback_ssot_3828", "AC4 build.py", build)
    must("Issue #3828", "AC4 test cite", t)
    if (ROOT / "tests" / "compiler" / "test_issue_3828.cpp").is_file():
        fails.append("AC4: test_issue_3828.cpp present (forbidden invent)")
    if (ROOT / "tests" / "issues" / "test_issue_3828.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3828.cpp present")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3828-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print(f"Issue #3828 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3828 mutate:from-verification-feedback add_mutate SSOT — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
