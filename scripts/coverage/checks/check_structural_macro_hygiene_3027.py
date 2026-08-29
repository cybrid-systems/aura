#!/usr/bin/env python3
"""Issue #3027: residual MacroIntroduced gates on structural mutate prims.

set-body / remove-node / insert-child / splice / wrap / extract-function /
inline-call default-reject MacroIntroduced unless :allow-macro? (or global).
extract-function never stamps MacroIntroduced without allow.
Lockless atomic-batch paths reject (no :allow-macro? on batch).

Contract (one row per AC):
  AC1  public prims reject MacroIntroduced via reject_structural_macro_hygiene
  AC2  :allow-macro? / global permit + restamp on allowed path
  AC3  extract-function stamps only after allow; never invents markers
  AC4  Soft / non-macro unchanged; lockless batch reject; no new query keys
  AC5  tests + build.py; no invent / docs/design; not a second hygiene model

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
        pos = src.find(f'add_mutate(\n        "{name}"')
    if pos < 0:
        pos = src.find(f"── {name} ")
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

    # AC1 public gates
    must("Issue #3027", "AC1 helper", mut)
    must("reject_structural_macro_hygiene", "AC1 helper", mut)
    must("cannot ", "AC1 stable msg", mut)
    must(":allow-macro? #t", "AC1 allow msg", mut)
    for prim, needle in (
        ("mutate:set-body", "set-body"),
        ("mutate:remove-node", "remove-node"),
        ("mutate:insert-child", "insert-child"),
        ("mutate:splice", "splice"),
        ("mutate:wrap", "wrap"),
        ("mutate:extract-function", "extract-function"),
        ("mutate:inline-call", "inline-call"),
    ):
        win = _prim_win(mut, prim)
        must("reject_structural_macro_hygiene", f"AC1 {prim}", win)
        must(needle, f"AC1 {prim} name", win)
    must("3027 AC1", "AC1 test", test)
    must("ac3027_1_default_reject_all_prims", "AC1 test fn", test)

    # AC2 allow + restamp
    must("parse_allow_macro_opt_out", "AC2 set-body", _prim_win(mut, "mutate:set-body", 14000))
    must("propagate_macro_introduced_marker", "AC2 set-body restamp", _prim_win(mut, "mutate:set-body", 14000))
    must("propagate_macro_introduced_marker", "AC2 wrap restamp", _prim_win(mut, "mutate:wrap", 12000))
    must("3027 AC2", "AC2 test", test)
    must("ac3027_2_allow_macro_permits", "AC2 test fn", test)

    # AC3 extract stamp only after allow
    ex = _prim_win(mut, "mutate:extract-function", 12000)
    must("stamp_macro", "AC3 stamp flag", ex)
    must("allow_macro_ex && target_was_macro", "AC3 stamp cond", ex)
    must("set_marker", "AC3 set_marker after allow", ex)
    # set_marker must not run unconditionally — the old always-stamp is gone.
    if "flat.set_marker(define_id, SyntaxMarker::MacroIntroduced);" in ex:
        # must be inside stamp_macro branch
        idx = ex.find("flat.set_marker(define_id, SyntaxMarker::MacroIntroduced);")
        before = ex[max(0, idx - 200) : idx]
        if "stamp_macro" not in before and "if (stamp_macro)" not in before:
            fails.append("AC3: extract still stamps define unconditionally")
    must("3027 AC3", "AC3 test", test)
    must("ac3027_3_extract_no_stamp_without_allow", "AC3 test fn", test)

    # AC4 Soft + lockless + no new query
    must("3027 AC4", "AC4 test", test)
    must("Issue #3027", "AC4 lockless", flat)
    must("batch :remove-node: cannot remove-node MacroIntroduced", "AC4 lockless rm", flat)
    must("batch :set-body: cannot set-body MacroIntroduced", "AC4 lockless sb", flat)
    must("batch :insert-child: cannot insert-child MacroIntroduced", "AC4 lockless ins", flat)
    must("batch :splice: cannot splice MacroIntroduced", "AC4 lockless spl", flat)
    must("batch :wrap: cannot wrap MacroIntroduced", "AC4 lockless wrap", flat)
    must("batch :inline-call: cannot inline-call MacroIntroduced", "AC4 lockless inl", flat)
    if "query:structural-macro-hygiene" in mut:
        fails.append("AC4: new top-level query key (forbidden)")

    # AC5 wiring
    must("check_structural_macro_hygiene_3027", "AC5 build", build)
    must("cmd_structural_macro_hygiene_3027", "AC5 build cmd", build)
    must("ac3027_5_source_and_linter", "AC5 test fn", test)
    cite = mut.find("Issue #3027")
    if cite >= 0 and "AgentRegistry" in mut[cite : cite + 2500]:
        fails.append("AC5: must not introduce AgentRegistry")
    if (ROOT / "tests" / "compiler" / "test_issue_3027.cpp").is_file():
        fails.append("AC5: test_issue_3027.cpp present (forbidden per #81967)")
    if _read("docs/design/3027-structural-macro-hygiene.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print(f"Issue #3027 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3027 residual structural MacroIntroduced gates — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
