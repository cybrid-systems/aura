#!/usr/bin/env python3
"""Issue #3453: set_child_locked equal-length patches dense children in-place.

#3402 closed the read-path layout. Every structural mutator still only
set dense_dirty_=true, so the next children_columnar full-rebuilt
child_data_. Equal-length set_child now writes the synced slot in O(1).
insert/remove keep dirty (arity). No new query key.

Contract:
  AC1 equal-length set on synced tree leaves dense_dirty_ false
  AC2 insert/remove still dirty
  AC3 copy/compact/restore still force dirty
  AC4 no new query key; exclusive/COW counters unchanged
  AC5 no docs/design/3453-*; no test_issue_3453.cpp

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

    ast = _read("src/core/ast.ixx")
    t = _read("tests/core/test_arena_required_cover_no_value_only.cpp")
    l3402 = _read("scripts/check_dense_children_columns_3402.py")
    build = _read("build.py")

    set_idx = ast.find("void set_child_locked(")
    swin = ast[set_idx : set_idx + 2800] if set_idx >= 0 else ""
    must("Issue #3453", "AC1 cite", swin)
    must("!dense_dirty_", "AC1 in-place gate", swin)
    must("child_data_[", "AC1 slot write", swin)
    brace = swin.find("{")
    head = swin[brace : brace + 200] if brace >= 0 else ""
    if "dense_dirty_ = true" in head:
        fails.append("AC1: set_child_locked still unconditionally dirties at entry")
    must("ac3453_equal_length_set_inplace", "AC1 test", t)
    must("AC3453: set_child_locked", "AC1 extend 3402 linter", l3402)

    ins = ast.find("void insert_child_locked(")
    rem = ast.find("void remove_child_locked(")
    iwin = ast[ins : ins + 400] if ins >= 0 else ""
    rwin = ast[rem : rem + 400] if rem >= 0 else ""
    must("dense_dirty_ = true", "AC2 insert dirty", iwin)
    must("dense_dirty_ = true", "AC2 remove dirty", rwin)

    must("Issue #3402: dest keeps its own runtime_resource_", "AC3 copy/move", ast)
    must("Issue #3402: compact remaps NodeIds", "AC3 compact", ast)
    must("Issue #3402: PCV snapshot is the source of truth", "AC3 restore", ast)

    must("flatast_locked_move_out_exclusive_total", "AC4 exclusive counter", swin)
    must("flatast_locked_move_out_cow_total", "AC4 cow counter", swin)
    if "schema-3453" in ast or "schema-3453" in t:
        fails.append("AC4: new schema-3453 query key")
    must("kSetChildLockedDenseInplaceIssue = 3453", "AC4 stamp", ast)

    must("check_set_child_locked_dense_inplace_3453", "AC5 build.py", build)
    must("check_dense_children_columns_3402", "AC5 #3402 retained", build)
    if (ROOT / "tests" / "core" / "test_issue_3453.cpp").is_file():
        fails.append("AC5: forbidden tests/core/test_issue_3453.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3453.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3453.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3453-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL #3453 set_child_locked_dense_inplace:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3453 set_child_locked_dense_inplace: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
