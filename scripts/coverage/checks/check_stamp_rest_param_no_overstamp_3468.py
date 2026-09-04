#!/usr/bin/env python3
"""Issue #3468: stamp_rest_param_hygiene stamps spine only, not remaining args.

#3153 closed the under-stamp residual. The helper still DFS-walked
list_root children, so caller remaining NodeIds got MacroIntroduced.

Contract:
  AC1 helper stamps Call+head (or pair cells along cdr); no remaining DFS
  AC2 remaining Literal/Variable keep pre-stamp marker
  AC3 mutate remaining without :allow-macro? is not hygiene-macro-introduced
      solely from rest wrap; list Call still rejects
  AC4 no new query key / metric atomic
  AC5 Soft/Off unchanged (no extra Soft walk)
  AC6 extend existing rest-param suites; no test_issue_3468.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    me = (ROOT / "src" / "compiler" / "macro_expansion.cpp").read_text()
    marker = (ROOT / "tests" / "compiler" / "test_stamp_rest_param_hygiene_marker.cpp").read_text()
    eef_test = (ROOT / "tests" / "compiler" / "test_rest_param_hygiene_eval_flat.cpp").read_text()
    hyg = (ROOT / "tests" / "compiler" / "test_hygiene_mutate_closed_loop.cpp").read_text()
    mutate = (ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp").read_text()

    pos = me.find("inline void stamp_rest_param_hygiene(aura::ast::FlatAST&")
    if pos < 0:
        fails.append("AC1: stamp_rest_param_hygiene missing")
        win = ""
    else:
        win = me[pos : pos + 2800]
    must("Issue #3468", "AC1 cite", win)
    must("Issue #2808", "AC1 #2808 marker kept", win)
    must("stamp_one(root_v.child(0))", "AC1 Call+head", win)
    must("NodeTag::Pair", "AC1 pair spine", win)
    must_not("stack.push_back(child)", "AC1 no remaining DFS", win)
    must("set_marker", "AC1 set_marker", win)
    must("g_stamp_rest_param_marker_set_total", "AC4 reuse set total", win)

    must("3468: remaining a1 keeps User marker", "AC2 marker test", marker)
    must("3468: remaining car a1 stays User", "AC2 pair car", marker)
    must("3468: stamp spine only, not remaining args", "AC2 eval_flat", eef_test)
    must("3468: remaining arg not hygiene-macro-introduced", "AC3 mutate remaining", hyg)
    must("3468: mutate list spine still hygiene-macro-introduced", "AC3 mutate spine", hyg)

    must_not("schema-3468", "AC4 no new query key", mutate)
    if "g_3468_" in me:
        fails.append("AC4: new g_3468_* atomic")
    must("Soft / Off", "AC5 contract comment", eef_test)

    if (ROOT / "tests" / "compiler" / "test_issue_3468.cpp").is_file():
        fails.append("AC6: forbidden tests/compiler/test_issue_3468.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3468-*")):
            fails.append(f"AC6: docs/design/{f.name} present")

    if fails:
        print("FAIL #3468 stamp_rest_param_no_overstamp:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3468 stamp_rest_param_no_overstamp: spine only, remaining User")
    return 0


if __name__ == "__main__":
    sys.exit(main())
