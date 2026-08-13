#!/usr/bin/env python3
"""Issue #2906: FlatAST locked mutate forces exclusive PCV via move-out.

Contract:
  AC1 set_child/insert/remove_locked move children out before cow_*
  AC2 flatast_locked_move_out_exclusive_total metric + sole-holder stress tests
  AC3 SafePCVSpan / snapshot still force COW (rollback keeps with_* semantics)
  AC4 schema-2906 query keys; no docs/design/*; no new test file

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

    hh = _read("src/core/persistent_child_vector.hh")
    ast = _read("src/core/ast.ixx")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/core/test_pcv_exclusive_with_set.cpp")
    build = _read("build.py")

    # AC1: locked mutate paths move-out + cow_* + move-back.
    must("#2906", "AC1", hh)
    must("#2906", "AC1", ast)
    must("std::move(children_[id])", "AC1 set", ast)
    must("list.cow_set", "AC1 set", ast)
    must("list.cow_insert", "AC1 insert", ast)
    must("list.cow_erase", "AC1 erase", ast)
    must("flatast_locked_move_out_exclusive_total", "AC1 metric", hh)
    must("flatast_locked_move_out_exclusive_total", "AC1 bump", ast)
    must("#2906", "AC1 mutate", mut)

    # AC2: metrics + sole-holder stress in test.
    must("flatast_locked_move_out_exclusive_total", "AC2", hh)
    must("flatast_locked_move_out_cow_total", "AC2", hh)
    must("kPcvFlatastLockedExclusiveIssue", "AC2", hh)
    must("#2906 AC2", "AC2 test", test)

    # AC3: SafePCVSpan forces COW; rollback keeps with_* (correctness first).
    must("SafePCVSpan", "AC3", test)
    must("flatast_locked_move_out_cow_total", "AC3", ast)
    must("use_count()>1", "AC3", hh)
    must("with_set", "AC3 rollback", ast)  # rollback keeps with_* COW path

    # AC4: query keys wired; linter in build.py; no docs/design; no new test.
    must("schema-2906", "AC4", obs)
    must("flatast-locked-move-out-exclusive-total", "AC4", obs)
    must("flatast-locked-exclusive-ratio-bp", "AC4", obs)
    must("check_pcv_flatast_locked_exclusive_2906", "AC4", build)
    must("cmd_pcv_flatast_locked_exclusive_2906", "AC4", build)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2906-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "core" / "test_issue_2906.cpp").is_file():
        fails.append("tests/core/test_issue_2906.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2906 FlatAST locked PCV exclusive move-out — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
