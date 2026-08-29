#!/usr/bin/env python3
"""Issue #3061: move-node / replace-subtree target honor :allow-macro?

Unify both sites on reject_structural_macro_hygiene + parse_allow_macro_opt_out.
Default still hygiene-rejects. Soft / non-macro unchanged. No new query keys.

  AC1  public move-node uses shared helper + :allow-macro? / global
  AC2  public replace-subtree *target* uses the same helper
  AC3  lockless batch honors get_allow_macro_mutate; deny still metrics
  AC4  after allow, propagate_macro_introduced_marker / restamp
  AC5  extend existing suites; no test_issue_3061.cpp; no docs/design/

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
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    tmove = _read("tests/compiler/test_move_node_hygiene.cpp")
    trepl = _read("tests/compiler/test_replace_subtree_new_body_hygiene.cpp")
    build = _read("build.py")

    ppos = mut.find('add_mutate("mutate:move-node"')
    if ppos < 0:
        ppos = mut.find('add_mutate(\n        "mutate:move-node"')
    if ppos < 0:
        ppos = mut.find("── mutate:move-node")
    if ppos < 0:
        ppos = mut.find("mutate:move-node")
    pwin = mut[ppos : ppos + 12000] if ppos >= 0 else ""

    rpos = mut.find('add_mutate("mutate:replace-subtree"')
    if rpos < 0:
        rpos = mut.find('add_mutate(\n        "mutate:replace-subtree"')
    if rpos < 0:
        rpos = mut.find("── mutate:replace-subtree")
    if rpos < 0:
        rpos = mut.find("mutate:replace-subtree")
    rwin = mut[rpos : rpos + 14000] if rpos >= 0 else ""

    lpos = flat.find("eval_flat_apply_mutate_move_node")
    lwin = flat[lpos : lpos + 2800] if lpos >= 0 else ""
    lrpos = flat.find("eval_flat_apply_mutate_replace_subtree")
    lrwin = flat[lrpos : lrpos + 2800] if lrpos >= 0 else ""

    # AC1 move-node public
    must("Issue #3061", "AC1 move", pwin)
    must("reject_structural_macro_hygiene", "AC1 helper", pwin)
    must("parse_allow_macro_opt_out", "AC1 opt-out", pwin)
    must("note_move_node_hygiene_reject", "AC1 deny metric", pwin)
    must("3061 AC1", "AC1 test", tmove)

    # AC2 replace-subtree target
    must("Issue #3061", "AC2 replace", rwin)
    must("reject_structural_macro_hygiene", "AC2 helper", rwin)
    must('"replace-subtree"', "AC2 prim name", rwin)
    must("3061 AC1", "AC2 test", trepl)

    # AC3 lockless
    must("get_allow_macro_mutate", "AC3 move batch", lwin)
    must("note_move_node_hygiene_reject", "AC3 move metric", lwin)
    must("cannot move macro-introduced", "AC3 move deny msg", lwin)
    must("get_allow_macro_mutate", "AC3 replace batch", lrwin)
    must("3061 AC3", "AC3 batch test", tmove)

    # AC4 propagate after allowed mutate
    must("propagate_macro_introduced_marker", "AC4 move", pwin)
    # replace-subtree body is long; search from the #3061 target gate.
    rs3061 = mut.find("Issue #3061 / #142 / #3027")
    rprop = mut[rs3061 : rs3061 + 20000] if rs3061 >= 0 else ""
    must("propagate_macro_introduced_marker", "AC4 replace", rprop)

    # AC5
    must("check_move_replace_allow_macro_3061", "AC5 build", build)
    must("cmd_move_replace_allow_macro_3061", "AC5 cmd", build)
    must("3061 AC5", "AC5 move test", tmove)
    if (ROOT / "tests" / "compiler" / "test_issue_3061.cpp").is_file():
        fails.append("AC5: test_issue_3061.cpp present (forbidden per #81967)")
    if _read("docs/design/3061-move-replace-allow-macro.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print(f"Issue #3061 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3061 move/replace-subtree :allow-macro? — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
