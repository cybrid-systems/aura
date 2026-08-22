#!/usr/bin/env python3
"""Issue #3213: lockless atomic-batch dual-track :allow-macro?.

Public structural/scalar prims honor get_allow_macro_mutate() OR
parse_allow_macro_opt_out (`:allow-macro? #t`). Lockless
eval_flat_apply_mutate_* previously consulted only the global flag
(or always-rejected MacroIntroduced). Residual closed: every
MacroIntroduced gate on the lockless path shares the public parse.

Contract (one row per AC):
  AC1  every eval_flat_apply_mutate_* that gates on MacroIntroduced
       parses :allow-macro? (Evaluator::parse_allow_macro_opt_out)
  AC2  :allow-macro? #t in batch op args does not require the global flag
  AC3  default (no keyword, global=false) still rejects
  AC4  atomic-batch surgical opt-in of one MacroIntroduced op; sibling denied
  AC5  Soft/Off: no extra parse when not MacroIntroduced / allow already true
  AC6  tests extend hygiene_mutate_closed_loop; linter wired; no invent

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

GATED = (
    "replace_value",
    "tweak_literal",
    "remove_node",
    "insert_child",
    "set_body",
    "replace_pattern",
    "replace_subtree",
    "splice",
    "wrap",
    "rename_symbol",
    "move_node",
    "inline_call",
)


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _fn_win(src: str, name: str) -> str:
    key = f"eval_flat_apply_mutate_{name}"
    a = src.find(key)
    if a < 0:
        return ""
    nxt = src.find("EvalResult Evaluator::eval_flat_apply_mutate_", a + len(key))
    end = nxt if nxt > a else a + 8000
    return src[a:end]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    hdr = _read("src/compiler/evaluator.ixx")
    test = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")

    # AC1 — shared parse on every lockless MacroIntroduced gate.
    must("Issue #3213", "AC1 eval_flat cite", flat)
    must("bool Evaluator::parse_allow_macro_opt_out", "AC1 member def", flat)
    must("parse_allow_macro_opt_out", "AC1 Evaluator decl", hdr)
    must("Issue #3213", "AC1 mutate cite", mut)
    must("return ev.parse_allow_macro_opt_out(args)", "AC1 thin wrap", mut)
    for name in GATED:
        win = _fn_win(flat, name)
        if not win:
            fails.append(f"AC1: missing eval_flat_apply_mutate_{name}")
            continue
        if "parse_allow_macro_opt_out" not in win:
            fails.append(f"AC1: {name} missing parse_allow_macro_opt_out")
        if "get_allow_macro_mutate()" not in win:
            fails.append(f"AC1: {name} missing get_allow_macro_mutate()")
    must("3213 AC1", "AC1 test", test)

    # AC2 — per-op opt-in without global flag.
    must("Issue #3213", "AC2 cite", flat)
    must(":allow-macro? #t", "AC2 kwarg", flat)
    must("3213 AC2", "AC2 test", test)
    must("global still false", "AC2 test global", test)

    # AC3 — default-deny unchanged.
    must("record_hygiene_violation_attempt()", "AC3 deny counter", flat)
    must("3213 AC3", "AC3 test", test)
    must("value unchanged after default-deny", "AC3 test deny", test)

    # AC4 — surgical one-op opt-in, sibling denied.
    must("3213 AC4", "AC4 test", test)
    must("3213-mixed", "AC4 mixed batch", test)
    must(":allow-macro? #t", "AC4 opt-in kwarg in test", test)

    # AC5 — Soft/Off short-circuit: is_macro_introduced first, then
    # get_allow_macro_mutate() || parse (C++ || skips parse when allow).
    must(
        "get_allow_macro_mutate() || parse_allow_macro_opt_out(a)",
        "AC5 || short-circuit",
        flat,
    )
    must("is_macro_introduced(node) &&", "AC5 is_macro_introduced first", flat)
    must("3213 AC5", "AC5 test", test)
    must("non-macro replace-value commits", "AC5 non-macro", test)

    # AC6 — suite + linter + no invent.
    must("ac3213_1_source_all_gates_parse", "AC6 AC1 fn", test)
    must("ac3213_2_per_op_opt_in_no_global", "AC6 AC2 fn", test)
    must("ac3213_3_default_deny", "AC6 AC3 fn", test)
    must("ac3213_4_surgical_sibling_denied", "AC6 AC4 fn", test)
    must("ac3213_5_soft_non_macro_zero_extra", "AC6 AC5 fn", test)
    must("ac3213_6_linter_no_docs", "AC6 AC6 fn", test)
    must("check_atomic_batch_allow_macro_3213", "AC6 build.py", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3213.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_3213.cpp present (forbidden #81967)")
    if (ROOT / "tests" / "issues" / "test_issue_3213.cpp").is_file():
        fails.append("AC6: tests/issues/test_issue_3213.cpp present (forbidden #81967)")
    if _read("docs/design/3213-atomic-batch-allow-macro.md"):
        fails.append("AC6: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3213 atomic_batch_allow_macro:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3213 atomic_batch_allow_macro: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
