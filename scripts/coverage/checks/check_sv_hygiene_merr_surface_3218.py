#!/usr/bin/env python3
"""Issue #3218: SV prims hygiene deny is merr("hygiene"), not bool false.

After #3131 / #3191 closed MacroIntroduced default-deny on
mutate:sv-add-coverpoint / mutate:sv-weaken-property, the reject still
returned make_bool(false) while structural prims return
merr("hygiene", "cannot … without :allow-macro? #t"). Agent replay
cannot treat the reject uniformly.

Contract (one row per AC):
  AC1  both SV prims call reject_structural_macro_hygiene with their
       prim name + mev (merr kind hygiene, helper message)
  AC2  get_allow_macro_mutate() still unlocks (no :allow-macro? parse)
  AC3  helper preserves record_hygiene_violation_attempt + note
  AC4  Soft/Off: helper short-circuits on non-macro (one is_macro_introduced)
  AC5  test_hygiene_mutate_closed_loop ac3218_*; this linter in build.py;
       no docs/design/3218-*; no test_issue_3218.cpp

No new query:* name. Reuses reject_structural_macro_hygiene.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _prim_win(src: str, name: str, n: int = 4000) -> str:
    key = f'add_mutate("{name}"'
    pos = src.find(key)
    return src[pos : pos + n] if pos >= 0 else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    t = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")

    helper = mut
    must("reject_structural_macro_hygiene", "AC1 helper", helper)
    must('mev("hygiene"', "AC1 merr kind", helper)
    must("cannot ", "AC1 cannot message", helper)
    must("without :allow-macro? #t", "AC1 allow-macro message", helper)

    sac = _prim_win(mut, "mutate:sv-add-coverpoint")
    swp = _prim_win(mut, "mutate:sv-weaken-property")
    must("Issue #3218", "AC1 coverpoint cite", sac)
    must("reject_structural_macro_hygiene", "AC1 coverpoint helper", sac)
    must('"sv-add-coverpoint"', "AC1 coverpoint prim name", sac)
    must("Issue #3218", "AC1 weaken cite", swp)
    must("reject_structural_macro_hygiene", "AC1 weaken helper", swp)
    must('"sv-weaken-property"', "AC1 weaken prim name", swp)

    # Dual-track residual: hygiene gate must not return make_bool(false).
    for label, win in (("sv-add-coverpoint", sac), ("sv-weaken-property", swp)):
        gate = win
        # Truncate at make_ref_layout so later success-path bools stay out.
        cut = gate.find("make_ref_layout")
        if cut >= 0:
            gate = gate[:cut]
        if (
            "return make_bool(false)" in gate
            and "is_macro_introduced" in gate
            and "reject_structural_macro_hygiene" not in gate
        ):
            fails.append(f"AC1: {label} still returns bool false on hygiene deny")

    must("ev.get_allow_macro_mutate()", "AC2 coverpoint allow", sac)
    must("ev.get_allow_macro_mutate()", "AC2 weaken allow", swp)
    if "parse_allow_macro_opt_out" in sac or "parse_allow_macro_opt_out" in swp:
        fails.append("AC2: :allow-macro? parse on SV prims (non-goal)")

    must("record_hygiene_violation_attempt", "AC3 helper counter", helper)
    must("kHygieneLimitReasonMacroIntroduced", "AC3 helper note", helper)
    must("note_hygiene_last_limit_reason", "AC3 helper note call", helper)

    must("is_macro_introduced", "AC4 helper short-circuit", helper)
    must("if (allow)", "AC4 allow short-circuit", helper)

    must("ac3218_1_sv_merr_hygiene", "AC5 test AC1", t)
    must("ac3218_2_allow_still_succeeds", "AC5 test AC2", t)
    must("ac3218_3_counters_and_soft", "AC5 test AC3/AC4", t)
    must("ac3218_5_source_and_linter", "AC5 test source", t)
    must("check_sv_hygiene_merr_surface_3218", "AC5 build.py", build)

    if "query:sv-hygiene" in mut or "query:sv-merr" in mut:
        fails.append("AC: new query:* name (reuse existing hygiene merr)")

    if (ROOT / "tests" / "compiler" / "test_issue_3218.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3218.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3218.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3218.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3218-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3218 sv_hygiene_merr_surface:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3218 sv_hygiene_merr_surface: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
