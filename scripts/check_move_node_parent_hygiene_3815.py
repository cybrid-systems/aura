#!/usr/bin/env python3
"""Issue #3815: mutate:move-node gates MacroIntroduced new_parent spine.

Public + lockless + atomic-batch pre-walk must refuse User→MacroIntroduced
parent without :allow-macro? / MacroSelfEvo (parity with insert-child /
splice). Soft/Off: zero extra when neither node nor parent is MacroIntroduced.

Contract (one row per AC):
  AC1  Public gates new_parent via reject_structural_macro_hygiene
  AC2  Lockless dual-track + #3652 MSE on new_parent
  AC3  Batch parent_arg=1 pre-walk for move-node
  AC4  Soft/Off zero extra when neither MacroIntroduced; tests extend
       test_move_node_hygiene (#81967); no invent / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    test = _read("tests/compiler/test_move_node_hygiene.cpp")
    build = _read("build.py")

    # Locate public move-node window
    ppos = mut.find("── mutate:move-node")
    if ppos < 0:
        ppos = mut.find('add_mutate(\n        "mutate:move-node"')
    if ppos < 0:
        ppos = mut.find("mutate:move-node")
    pwin = mut[ppos : ppos + 10000] if ppos >= 0 else ""

    lpos = flat.find("eval_flat_apply_mutate_move_node")
    lwin = flat[lpos : lpos + 5000] if lpos >= 0 else ""

    # AC1 public parent gate
    must("Issue #3815", "AC1 public cite", pwin)
    must("reject_structural_macro_hygiene", "AC1 public helper", pwin)
    must("parent_was_macro_mv", "AC1 parent_was_macro_mv", pwin)
    must("new_parent", "AC1 new_parent in gate", pwin)
    # Two reject_structural_macro_hygiene calls in the window (node + parent)
    if pwin.count("reject_structural_macro_hygiene") < 2:
        fails.append("AC1: public move-node must gate node AND new_parent")

    # AC2 lockless
    must("Issue #3815", "AC2 lockless cite", lwin)
    must("is_macro_introduced(new_parent)", "AC2 lockless parent gate", lwin)
    must("deny_macro_opt_out_without_mse", "AC2 MSE face", lwin)
    must("cannot move into MacroIntroduced parent", "AC2 parent deny msg", lwin)

    # AC3 batch parent_arg
    must("parent_arg", "AC3 struct field", mut)
    must("Issue #3815", "AC3 parent_arg cite", mut)
    must(
        '{"mutate:move-node", &Evaluator::eval_flat_apply_mutate_move_node, 0, 1}',
        "AC3 table row parent_arg=1",
        mut,
    )
    must("spine_args", "AC3 pre-walk spine_args", mut)

    # AC4 Soft/Off + tests + no invent
    must("3815 AC1", "AC4 test AC1", test)
    must("3815 AC3", "AC4 test Soft/Off", test)
    must("3815 AC4", "AC4 test batch", test)
    must("Issue #3815", "AC4 test cite", test)
    must("check_move_node_parent_hygiene_3815", "AC4 build wire", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3815.cpp").is_file():
        fails.append("AC4: invent test_issue_3815.cpp forbidden")
    if (ROOT / "docs" / "design" / "3815-move-node-parent-hygiene.md").is_file():
        fails.append("AC4: docs/design/3815-* forbidden")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"Issue #3815 move-node parent hygiene: {len(fails)} failure(s)", file=sys.stderr)
        return 1
    print("OK: Issue #3815 move-node parent hygiene")
    return 0


if __name__ == "__main__":
    sys.exit(main())
