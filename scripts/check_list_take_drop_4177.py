#!/usr/bin/env python3
"""Issue #4177: R7RS list take/drop — native bindings flipped to (lst k).

Native take/drop were registered count-first ((take k lst) with {Int, Dyn}
typed rows), so every R7RS-order call ((take lst k)) fell into the count
guard and returned an opaque primitive-error value: display rendered
<unknown> and pair? answered #f — the binding was present but unusable for
list prefix/suffix (Soft oneshot lacked working list-take/list-drop). The
fix flips both prims plus the type-checker rows to the R7RS/SRFI-1 (lst k)
order, keeps the count guard ahead of the list guard, and flips the pinned
count-first fixture rows.

Contract:
  AC1 evaluator prims cite #4177 and keep both guard arms per list/type
  AC2 type-checker rows are (Dyn, Int) for take/drop; no {Int, Dyn} rows left
  AC3 pinned suite fixtures flipped to R7RS order (2873 prim row + 2914 arms)
  AC4 runtime ACs live in test_primcall_narg.cpp (#4177 AC helpers + calls)
  AC5 build.py wiring + root_check_allowlist entry; no docs/design/4177-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    evp = _read("src/compiler/evaluator_primitives_list.cpp")
    must("Issue #4177", "AC1", evp)
    must('"take: count must be a non-negative integer"', "AC1", evp)
    must('"take: not a list"', "AC1", evp)
    must('"drop: count must be a non-negative integer"', "AC1", evp)
    must('"drop: not a list"', "AC1", evp)

    tc = _read("src/compiler/type_checker_impl.cpp")
    must('register_primitive("take", {Dyn, Int}, Dyn)', "AC2", tc)
    must('register_primitive("drop", {Dyn, Int}, Dyn)', "AC2", tc)
    must('register_poly_primitive("take", {Dyn, Int}, Dyn, {_a})', "AC2", tc)
    must('register_poly_primitive("drop", {Dyn, Int}, Dyn, {_a})', "AC2", tc)
    must_not('register_primitive("take", {Int, Dyn}, Dyn)', "AC2", tc)
    must_not('register_primitive("drop", {Int, Dyn}, Dyn)', "AC2", tc)
    must_not('register_poly_primitive("take", {Int, Dyn}, Dyn, {_a})', "AC2", tc)
    must_not('register_poly_primitive("drop", {Int, Dyn}, Dyn, {_a})', "AC2", tc)

    mfn = _read("tests/suite/multiframe_named_let_2873.aura")
    must("(take (list 9 8 7 6) 2)", "AC3", mfn)
    must_not("(take 2 (list 9 8 7 6))", "AC3", mfn)
    qp = _read("tests/suite/query_primitives_split_2914.aura")
    must('(take (list 1) "x")', "AC3", qp)
    must_not('(take "x" (list 1))', "AC3", qp)
    must("(drop (list 1) 9)", "AC3", qp)
    must_not("(drop 9 (list 1))", "AC3", qp)

    test = _read("tests/compiler/test_primcall_narg.cpp")
    must("#4177", "AC4", test)
    must("ac12_take_r7rs", "AC4", test)
    must("ac13_drop_r7rs", "AC4", test)
    must("ac14_take_drop_boundaries", "AC4", test)
    must("ac15_take_drop_errors", "AC4", test)
    must("ac16_source_gate", "AC4", test)

    build = _read("build.py")
    must("check_list_take_drop_4177", "AC5", build)
    allow = _read("scripts/coverage/root_check_allowlist.txt")
    must("check_list_take_drop_4177.py", "AC5", allow)
    if (ROOT / "docs" / "design").is_dir() and list((ROOT / "docs" / "design").glob("4177-*")):
        fails.append("AC5: docs/design/4177-* exists (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("OK: Issue #4177 R7RS list take/drop — AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
