#!/usr/bin/env python3
"""Issue #2424: is_subtree_dirty_node bounds via dirty_.size() (not size()).

Contract:
  AC1 no size() bounds check in is_subtree_dirty_node; use dirty_.size()
  AC2 add_node grows dirty_ under dirty_column_mtx_; concurrent test wired
  AC3 dirty_nodes_in_range caps on dirty_.size(); non-racing semantics test
  AC4 invariant dirty_.size() == tag_.size() documented in add_node

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: unexpected {n!r}")

    ast = _read("src/core/ast.ixx")
    test = _read("tests/core/test_subtree_dirty_bounds.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    must("Issue #2424", "AC1", ast)
    must("is_subtree_dirty_node", "AC1", ast)
    # Extract is_subtree_dirty_node body; forbid bare size()/tag_.size() bounds.
    m = re.search(
        r"bool is_subtree_dirty_node\(NodeId id\) const noexcept \{(.*?)\}",
        ast,
        re.DOTALL,
    )
    if not m:
        fails.append("AC1: could not find is_subtree_dirty_node body")
    else:
        body = m.group(1)
        # Drop line comments so "do not call size()" does not false-positive.
        code = re.sub(r"//.*?$", "", body, flags=re.MULTILINE)
        # Bare size() only (not dirty_.size() / tag_.size()).
        if re.search(r"(?<!dirty_\.)(?<!tag_\.)\bsize\s*\(\s*\)", code):
            fails.append("AC1: is_subtree_dirty_node still calls bare size()")
        if "tag_.size()" in code:
            fails.append("AC1: is_subtree_dirty_node still uses tag_.size()")
        if "dirty_.size()" not in code:
            fails.append("AC1: is_subtree_dirty_node missing dirty_.size() bound")
    must("2424 AC1", "AC1", test)

    must("dirty_.push_back(0)", "AC2", ast)
    must("dirty_column_mtx_.mutable_get()", "AC2", ast)
    must("2424 AC2", "AC2", test)
    must("concurrent add_node", "AC2", test)

    must("dirty_nodes_in_range", "AC3", ast)
    must("2424 AC3", "AC3", test)

    must("dirty_.size() == tag_.size()", "AC4", ast)
    must("INVARIANT (Issue #2424", "AC4", ast)
    must("2424 AC4", "AC4", test)

    must("check_subtree_dirty_bounds_2424", "gate", build)
    must("cmd_subtree_dirty_bounds_coverage", "gate", build)
    must("test_subtree_dirty_bounds", "gate", cmake)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: subtree dirty bounds #2424 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
