#!/usr/bin/env python3
"""Issue #3665: insert/remove_child_locked splice dense children on a synced tree.

#3453 closed equal-length set_child_locked in-place. insert/remove still
set dense_dirty_=true, so the next children_columnar did child_data_.clear()
+ a full PCV walk. Arity-changing Agent mutate paid O(module) not O(arity).

Contract (one row per AC):
  AC1  synced insert_child_locked: !dense_dirty_; child_count_ +1;
       child_data_.insert; later child_begin_ += 1
  AC2  synced remove_child_locked: erase one slot; no full rebuild
  AC3  #3453 equal-length set in-place kept; compact/copy/restore still dirty
  AC4  Soft/Off same control flow; exclusive/COW counters still bump;
       no new query key
  AC5  extend #3402/#3453 suite; linter AFTER #3664; no invent; no docs/design

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    ast = _read("src/core/ast.ixx")
    t = _read("tests/core/test_arena_required_cover_no_value_only.cpp")
    l3402 = _read("scripts/check_dense_children_columns_3402.py")
    l3453 = _read("scripts/coverage/checks/check_set_child_locked_dense_inplace_3453.py")
    build = _read("build.py")
    qh = _read("src/compiler/evaluator_primitives_query.cpp")
    obs = _read("src/compiler/evaluator_primitives_observability.cpp")

    ins = ast.find("void insert_child_locked(")
    rem = ast.find("void remove_child_locked(")
    pub = ast.find("void set_child(NodeId", rem if rem >= 0 else 0)
    iwin = ast[ins:rem] if ins >= 0 and rem > ins else ""
    rwin = ast[rem:pub] if rem >= 0 and pub > rem else ""

    must("Issue #3665", "AC1 cite", iwin)
    must("!dense_dirty_", "AC1 splice gate", iwin)
    must("child_data_.insert", "AC1 splice insert", iwin)
    must("child_count_[id] = n + 1", "AC1 count +1", iwin)
    must("child_begin_[i] += 1", "AC1 later begins", iwin)
    brace = iwin.find("{")
    head = iwin[brace : brace + 200] if brace >= 0 else ""
    if "dense_dirty_ = true" in head:
        fails.append("AC1: insert_child_locked still unconditionally dirties at entry")
    must("kInsertRemoveChildLockedDenseSpliceIssue = 3665", "AC1 stamp", ast)
    must("ac3665_insert_remove_dense_splice", "AC1 test", t)
    must("3665 AC1: insert on synced tree leaves dense_dirty_ false", "AC1 runtime", t)

    must("Issue #3665", "AC2 cite", rwin)
    must("child_data_.erase", "AC2 splice erase", rwin)
    must("child_count_[id] = n - 1", "AC2 count -1", rwin)
    must("child_begin_[i] -= 1", "AC2 later begins", rwin)
    rbrace = rwin.find("{")
    rhead = rwin[rbrace : rbrace + 200] if rbrace >= 0 else ""
    if "dense_dirty_ = true" in rhead:
        fails.append("AC2: remove_child_locked still unconditionally dirties at entry")
    must("3665 AC2: remove on synced tree leaves dense_dirty_ false", "AC2 runtime", t)

    set_idx = ast.find("void set_child_locked(")
    swin = ast[set_idx : set_idx + 3600] if set_idx >= 0 else ""
    must("!dense_dirty_", "AC3 #3453 gate", swin)
    must("child_data_[", "AC3 #3453 slot write", swin)
    must("Issue #3402: compact remaps NodeIds", "AC3 compact", ast)
    must("Issue #3402: PCV snapshot is the source of truth", "AC3 restore", ast)
    must("Issue #3402: dest keeps its own runtime_resource_", "AC3 copy/move", ast)
    must("3665 AC3: #3453 equal-length set still in-place", "AC3 test", t)

    must("flatast_locked_move_out_exclusive_total", "AC4 exclusive", iwin)
    must("flatast_locked_move_out_cow_total", "AC4 cow", iwin)
    must("flatast_locked_move_out_exclusive_total", "AC4 remove exclusive", rwin)
    must("flatast_locked_move_out_cow_total", "AC4 remove cow", rwin)
    must_not("schema-3665", "AC4 no new query key", ast)
    must("query:type-linear-commit-health", "AC4 commit-health key", qh + obs)

    must("check_replace_pattern_match_sub_no_std_function_3664", "AC5 prev linter", build)
    must("check_insert_remove_child_locked_dense_splice_3665", "AC5 build.py", build)
    prev = build.find("check_replace_pattern_match_sub_no_std_function_3664")
    ours = build.find("check_insert_remove_child_locked_dense_splice_3665")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3664")
    must("AC3665", "AC5 #3402 linter extended", l3402)
    must("ac3665_insert_remove_dense_splice", "AC5 #3453 linter extended", l3453)
    if _read("tests/core/test_issue_3665.cpp") or _read("tests/compiler/test_issue_3665.cpp"):
        fails.append("AC5: test_issue_3665.cpp present")
    if (ROOT / "docs" / "design").is_dir():
        for f in sorted((ROOT / "docs" / "design").glob("3665-*")):
            fails.append(f"AC5: docs/design/{f.name} present")

    if fails:
        print("FAIL #3665 insert_remove_child_locked_dense_splice:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3665 insert_remove_child_locked_dense_splice")
    return 0


if __name__ == "__main__":
    sys.exit(main())
