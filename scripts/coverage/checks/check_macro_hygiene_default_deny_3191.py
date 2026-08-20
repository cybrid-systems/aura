#!/usr/bin/env python3
"""Issue #3191: post-#3131 default-deny residual on lockless tweak-literal
+ mutate:sv-add-coverpoint / mutate:sv-weaken-property.

Production contract: no MacroIntroduced path escapes the default-deny
(soft observe-only). Three gates close the residual: tweak-literal
(lockless batch table), sv-add-coverpoint, sv-weaken-property.
Global (hygiene:set-allow-macro-mutate! #t) still unlocks all three.
Soft/Off: zero extra cost on non-macro target (single atomic load).

Contract:
  AC1 tweak-literal on MacroIntroduced → reject (no mutation log entry)
  AC2 sv-add-coverpoint / sv-weaken-property on MacroIntroduced → reject
  AC3 global allow_macro_mutate still unlocks all three
  AC4 Soft / Off: zero extra cost on non-macro (single atomic load)
  AC5 existing #3027 / #3115 / #3131 scalar prims non-regression
  AC6 extend hygiene_mutate_closed_loop; no docs/design / invent;
     coverage linter wired into build.py

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

    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    t = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")

    # AC1 — tweak-literal default-deny gate.
    must("Issue #3191", "AC1", flat)
    must("is_macro_introduced(node)", "AC1", flat)
    must("get_allow_macro_mutate()", "AC1", flat)
    must("cannot tweak-literal MacroIntroduced", "AC1", flat)
    must("record_hygiene_violation_attempt()", "AC1", flat)

    # AC2 — sv-add-coverpoint / sv-weaken-property default-deny gates.
    must("Issue #3191", "AC2", mut)
    must("sv-add-coverpoint cannot", "AC2", mut)
    must("sv-weaken-property cannot", "AC2", mut)
    must("ev.get_allow_macro_mutate()", "AC2", mut)
    must("ev.record_hygiene_violation_attempt()", "AC2", mut)

    # AC3 — global allow still unlocks (existing parity with #3115 / #3027).
    must("set-allow-macro-mutate", "AC3", flat)  # noqa: irrelevant, but surfaces intent
    # The lockless batch has no :allow-macro? — global flag only, parity
    # with #3115 sv / #3027 structural prims.

    # AC4 — Soft / Off: single atomic load on non-macro (zero extra cost).
    # The pattern is: is_macro_introduced(load) + get_allow_macro_mutate(load),
    # both relaxed atomic loads. No extra metric / counter on the happy path.
    must("is_macro_introduced", "AC4", flat)
    must("is_macro_introduced", "AC4", mut)

    # AC5 — existing #3027 / #3076 / #3115 / #3131 surfaces preserved.
    must("reject_structural_macro_hygiene", "AC5", mut)
    must("Issue #3115", "AC5", mut)
    must("Issue #3027", "AC5", mut)
    must("Issue #3131", "AC5", mut)

    # AC6 — tests extend existing suite (no new test_issue_3191.cpp), no
    # docs/design/3191-* per #1655, linter wired into build.py.
    must("ac3191_1_default_reject", "AC6", t)
    must("ac3191_2_sv_default_reject", "AC6", t)
    must("ac3191_3_global_allow_unlocks", "AC6", t)
    must("ac3191_4_soft_non_macro_zero_cost", "AC6", t)
    must("ac3191_5_existing_surfaces_preserved", "AC6", t)
    must("ac3191_6_source_and_linter", "AC6", t)
    must("check_macro_hygiene_default_deny_3191", "AC6", build)

    if fails:
        print("FAIL #3191 macro_hygiene_default_deny:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3191 macro_hygiene_default_deny: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
