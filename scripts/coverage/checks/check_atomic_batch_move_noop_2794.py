#!/usr/bin/env python3
"""Issue #2794: atomic-batch bool-false is soft no-op; move-node same-pos commits.

Contract (one row per AC):
  AC1 atomic-batch cites #2794; #f → sub_op_noop_total + continue (not fail)
  AC2 lockless + EDSL move-node have already-at-destination no-op
  AC3 tests/compiler/test_atomic_batch_move_noop.cpp + no test_issue_2794.cpp
  AC4 this linter wired; no docs/design/2794-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


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

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    evh = _read("src/compiler/evaluator.ixx")
    test = _read("tests/compiler/test_atomic_batch_move_noop.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = mut.find('add_mutate("mutate:atomic-batch"')
    if pos < 0:
        pos = mut.find("mutate:atomic-batch")
    if pos < 0:
        fails.append("AC1: mutate:atomic-batch not found")
        ab_win = ""
    else:
        # Body spans pre-batch setup + full sub-op loop (~16KB+).
        ab_win = mut[pos : pos + 22000]

    # AC1: soft no-op path
    must("Issue #2794", "AC1", ab_win)
    must("sub_op_noop_total", "AC1", ab_win)
    must("continue", "AC1", ab_win)
    # Must NOT mark_sub_op_failed on the bool-false branch.
    bf = ab_win.find("is_bool(*sub_result)")
    if bf < 0:
        fails.append("AC1: bool sub_result check missing")
    else:
        bf_win = ab_win[max(0, bf - 100) : bf + 600]
        if "mark_sub_op_failed" in bf_win:
            fails.append("AC1: bool-false path still calls mark_sub_op_failed")
        if "sub_op_noop_total" not in bf_win:
            fails.append("AC1: bool-false path missing sub_op_noop_total bump")

    must("sub_op_noop_total", "AC1", evh)

    # AC2: move-node no-op both paths
    must("Issue #2794", "AC2", flat)
    if "cur_parent == new_parent" not in flat and "already at" not in flat.lower():
        fails.append("AC2: lockless move-node missing same-position no-op")
    # EDSL path
    mpos = mut.find('add_mutate("mutate:move-node"')
    if mpos < 0:
        mpos = mut.find("mutate:move-node")
    mwin = mut[mpos : mpos + 4000] if mpos >= 0 else ""
    must("Issue #2794", "AC2", mwin)
    if "cur_parent == new_parent" not in mwin:
        fails.append("AC2: EDSL move-node missing same-position no-op")

    # AC3
    must("ac2794", "AC3", test)
    must("2794", "AC3", test)
    must("move-node", "AC3", test)
    must("atomic-batch", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_atomic_batch_move_noop.cpp").is_file():
        fails.append("AC3: missing test_atomic_batch_move_noop.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2794.cpp").is_file():
        fails.append("AC3: test_issue_2794.cpp present (forbidden per #81967)")
    must("test_atomic_batch_move_noop", "AC3", cmake)

    # AC4
    must("check_atomic_batch_move_noop_2794", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2794-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2794 atomic-batch move-node no-op — #f soft no-op + same-position commit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
