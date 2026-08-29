#!/usr/bin/env python3
"""Issue #3402: FlatAST dense children columns + columnar walks over contiguous NodeId.

Contract:
  AC1 `FlatAST` declares child_data_ + child_begin_ + child_count_ dense
     columns (pmr::vector<...> triple) appended at struct END per the
     #2906/#3314 layout rule. Legacy children_ (vector<PCV>) is kept
     as edit buffer / snapshot anchor.
  AC2 walk_children_hot + children_columnar read contiguous NodeId
     via the dense columns — source-cite linter forbids `children_[id][`
     in those functions.
  AC3 children_columnar(id) lazy-syncs the dense columns from PCV on
     first call after a structural mutation (controlled by
     dense_dirty_), so callers see a consistent snapshot of
     post-mutation children without paying the sync cost up front.
  AC4 set_child_locked / insert_child_locked / remove_child_locked
     mark dense_dirty_ = true so the next children_columnar(id)
     triggers sync_dense_columns_from_pcv().
  AC5 sync_dense_columns_from_pcv() rebuilds child_data_ /
     child_begin_ / child_count_ from the legacy children_ vector
     (O(total children); runs once per structural-mutation batch).
  AC6 no tests/core/test_issue_3402.cpp (extends existing tests per
     #81934); no docs/design/3402-*.md (per #1655).
  AC7 source-cite #3402 + test/cmake/build gate; no design docs.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _strip_comments_and_strings(src: str) -> str:
    out = re.sub(r"//[^\n]*", "", src)
    out = re.sub(r"/\*.*?\*/", "", out, flags=re.S)
    out = re.sub(r'"(?:\\.|[^"\\])*"', '""', out)
    return out


def main() -> int:
    fails: list[str] = []

    ast_ixx = _read("src/core/ast.ixx")
    build = _read("build.py")
    ast_stripped = _strip_comments_and_strings(ast_ixx)

    # AC1: child_data_ + child_begin_ + child_count_ declared in FlatAST.
    # The struct END placement is enforced by the order-of-declaration
    # source-cite (dense columns must appear AFTER all the other
    # SoA pmr::vector<...> fields like parent_, tag_, int_val_, etc.).
    if "child_data_{&runtime_resource_};" not in ast_ixx:
        fails.append("AC1: FlatAST is missing child_data_ member (dense children column backing store not declared)")
    if "child_begin_{&runtime_resource_};" not in ast_ixx:
        fails.append("AC1: FlatAST is missing child_begin_ member (dense children start-index vector not declared)")
    if "child_count_{&runtime_resource_};" not in ast_ixx:
        fails.append("AC1: FlatAST is missing child_count_ member (dense children length vector not declared)")
    # Struct-END placement: child_data_ must come AFTER parent_
    # (the existing SoA column). If child_data_ appears before parent_,
    # the SoA layout invariant is violated.
    child_data_pos = ast_ixx.find("child_data_{&runtime_resource_}")
    parent_pos = ast_ixx.find("std::pmr::vector<NodeId> parent_;")
    if child_data_pos != -1 and parent_pos != -1 and child_data_pos < parent_pos:
        fails.append(
            "AC1: child_data_ appears BEFORE parent_ in FlatAST struct — "
            "violates #2906/#3314 append-at-struct-END layout rule"
        )

    # AC2: walk_children_hot + children_columnar must NOT use children_[id][
    # in the production code path. The legacy children_ vector is kept
    # as edit buffer only — those functions read dense columns.
    #
    # Locate walk_children_hot / children_columnar impls by their unique
    # Issue #3402 anchor comments, then scan the ~600 chars around each
    # anchor for `children_[id][` (which would violate the dense-only
    # read contract).
    walk_anchor = "// Issue #3402: children_columnar returns a SafePCVSpan"
    if walk_anchor not in ast_ixx:
        fails.append(
            "AC2: walk_children_hot / children_columnar #3402 source-cite "
            "anchor not found in ast.ixx (dense-column read path not "
            "documented)"
        )
    else:
        ast_ixx.find(walk_anchor)
        # Scan the *stripped* (comments + strings removed) ~1200 chars
        # after the anchor. The source-cite comment in children_columnar
        # itself contains the literal text "children_[id][" as a
        # forbidden-string example — using stripped text avoids that
        # false positive. The sync helper legitimately iterates
        # children_[i] / children_[i][j] for the dense rebuild — those
        # single + double subscripts inside the sync helper are OK;
        # only children_[id][ inside children_columnar /
        # walk_children_hot impls is forbidden.
        anchor_in_stripped = ast_stripped.find(walk_anchor)
        if anchor_in_stripped != -1:
            after_anchor = ast_stripped[anchor_in_stripped : anchor_in_stripped + 1200]
            if re.search(r"children_\[id\]\s*\[", after_anchor):
                fails.append(
                    "AC2: walk_children_hot / children_columnar uses "
                    "children_[id][ double-subscript (must read contiguous "
                    "NodeId via child_data_ instead)"
                )

    # AC3: children_columnar triggers sync when dense_dirty_ is set.
    if "if (dense_dirty_)" not in ast_ixx or "sync_dense_columns_from_pcv();" not in ast_ixx:
        fails.append(
            "AC3: children_columnar does not trigger "
            "sync_dense_columns_from_pcv() when dense_dirty_ is set "
            "(stale dense mirror after structural mutation)"
        )

    # AC4: set_child_locked / insert_child_locked / remove_child_locked
    # mark dense_dirty_ = true. Simple string-window check (regex
    # matching the `pre(id < children_.size())` contract clause
    # between `)` and `{` is brittle — the substring check is more
    # robust and tolerant of pre(...) reformulations).
    for fn in ("set_child_locked", "insert_child_locked", "remove_child_locked"):
        fn_idx = ast_stripped.find(f"void {fn}(")
        if fn_idx == -1:
            fails.append(f"AC4: {fn} definition not found in ast.ixx")
            continue
        window = ast_stripped[fn_idx : fn_idx + 500]
        if "dense_dirty_ = true" not in window:
            fails.append(
                f"AC4: {fn} does not mark dense_dirty_ = true "
                "(next children_columnar call would return stale dense data)"
            )

    # AC5: sync_dense_columns_from_pcv rebuilds child_data_ /
    # child_begin_ / child_count_ from children_.
    if "sync_dense_columns_from_pcv()" not in ast_ixx:
        fails.append("AC5: sync_dense_columns_from_pcv() helper not found in ast.ixx")
    if "child_begin_[i]" not in ast_ixx or "child_count_[i]" not in ast_ixx:
        fails.append(
            "AC5: sync_dense_columns_from_pcv does not populate child_begin_[i] / child_count_[i] from the PCV vector"
        )

    # AC6: no tests/core/test_issue_3402.cpp, no docs/design/3402-*.md.
    if (ROOT / "tests" / "core" / "test_issue_3402.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_3402.cpp exists — must extend existing test per #81934")
    if list((ROOT / "docs" / "design").glob("3402-*.md")):
        fails.append("AC6: docs/design/3402-*.md exists — design docs banned per #1655")

    # AC7: source-cite #3402 + build.py registration; no design docs.
    if "#3402" not in ast_ixx:
        fails.append("AC7: source-cite #3402 missing from ast.ixx")
    if "check_dense_children_columns_3402" not in build:
        fails.append("AC7: build.py does not register check_dense_children_columns_3402")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("PASS: #3402 FlatAST dense children columns contract satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
