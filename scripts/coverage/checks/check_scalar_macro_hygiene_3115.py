#!/usr/bin/env python3
"""Issue #3115: mutate:replace-type / replace-value MacroIntroduced gate.

Scalar field mutators must share reject_structural_macro_hygiene with
structural prims. Soft/Off stays zero-cost (helper short-circuits).

Contract (one row per AC):
  AC1  replace-type without :allow-macro? → kind hygiene + record_hygiene_violation_attempt
  AC2  replace-value same
  AC3  :allow-macro? #t / global allow succeeds + propagate_macro_introduced_marker
  AC4  existing structural tests; extend test_hygiene_mutate_closed_loop
  AC5  atomic-batch lockless replace-value rejects MacroIntroduced
  AC6  Soft/Off zero-cost; no new query keys; no second hygiene model

No test_issue_3115.cpp. No docs/design/3115-* per #1655.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _prim_win(src: str, name: str, n: int = 8000) -> str:
    key = f'add_mutate("{name}"'
    pos = src.find(key)
    if pos < 0:
        pos = src.find(name)
    return src[pos : pos + n] if pos >= 0 else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    test = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")

    rt = _prim_win(mut, "mutate:replace-type", 10000)
    rv = _prim_win(mut, "mutate:replace-value", 12000)

    must("Issue #3115", "AC1 cite replace-type", rt)
    must("reject_structural_macro_hygiene", "AC1 replace-type helper", rt)
    must("replace-type", "AC1 replace-type prim name", rt)
    must("parse_allow_macro_opt_out", "AC1 replace-type allow", rt)
    must("3115 AC1", "AC1 test", test)

    must("Issue #3115", "AC2 cite replace-value", rv)
    must("reject_structural_macro_hygiene", "AC2 replace-value helper", rv)
    must("replace-value", "AC2 replace-value prim name", rv)
    must("parse_allow_macro_opt_out", "AC2 replace-value allow", rv)
    must("3115 AC2", "AC2 test", test)

    must("propagate_macro_introduced_marker", "AC3 replace-type restamp", rt)
    must("propagate_macro_introduced_marker", "AC3 replace-value restamp", rv)
    must("3115 AC2: marker preserved", "AC3 test marker preserved", test)

    must("3115 AC4", "AC4 test", test)
    must("check_scalar_macro_hygiene_3115", "AC4 build.py", build)

    must("Issue #3115", "AC5 lockless cite", flat)
    must("cannot replace-value MacroIntroduced", "AC5 lockless reject", flat)
    must("record_hygiene_violation_attempt", "AC5 lockless counter", flat)
    must("3115 AC5", "AC5 test", test)

    must("is_macro_introduced", "AC6 short-circuit via helper", rt)
    must("3115 AC6", "AC6 test", test)
    must("no invent test per #81967", "AC6 no invent", test)

    if (ROOT / "tests" / "compiler" / "test_issue_3115.cpp").is_file():
        fails.append("AC: tests/compiler/test_issue_3115.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3115-*")):
            fails.append(f"AC: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3115 scalar MacroIntroduced hygiene — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
