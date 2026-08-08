#!/usr/bin/env python3
"""Issue #2800: replace-pattern multi-match StableNodeRef two-phase + stale metric.

Lockless path must collect StableNodeRef matches before any parse_to_flat
apply; apply uses is_valid_in + reverse parent-edge; metric tracks skips.

Contract (one row per AC):
  AC1 lockless two-phase make_ref_layout + is_valid_in + #2800; public cites
  AC2 replace_pattern_stale_nodeid_prevented metric on FlatAST
  AC3 tests/compiler/test_replace_pattern_multi_match_nodeid_stability.cpp
  AC4 this linter wired; no docs/design/2800-*; no test_issue_2800.cpp

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
    ast = _read("src/core/ast.ixx")
    test = _read("tests/compiler/test_replace_pattern_multi_match_nodeid_stability.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    lpos = flat.find("eval_flat_apply_mutate_replace_pattern")
    lwin = flat[lpos : lpos + 12000] if lpos >= 0 else ""

    ppos = mut.find('add_mutate("mutate:replace-pattern"')
    if ppos < 0:
        ppos = mut.find("mutate:replace-pattern")
    pwin = mut[ppos : ppos + 24000] if ppos >= 0 else ""

    # AC1
    must("Issue #2800", "AC1", lwin)
    must("make_ref_layout", "AC1", lwin)
    must("is_valid_in", "AC1", lwin)
    must("note_replace_pattern_stale_nodeid_prevented", "AC1", lwin)
    must("StableNodeRef", "AC1", lwin)
    # Collect before apply: vector of matches appears before begin_atomic_batch
    collect = lwin.find("vector<StableNodeRef> matches")
    if collect < 0:
        collect = lwin.find("std::vector<StableNodeRef> matches")
    batch = lwin.find("begin_atomic_batch")
    if collect < 0 or batch < 0 or collect > batch:
        fails.append("AC1: StableNodeRef matches not collected before begin_atomic_batch")
    must("Issue #2800", "AC1", pwin)
    must("stable_match_still_attached", "AC1", pwin)
    must("note_replace_pattern_stale_nodeid_prevented", "AC1", pwin)

    # AC2
    must("replace_pattern_stale_nodeid_prevented", "AC2", ast)
    must("Issue #2800", "AC2", ast)
    must("note_replace_pattern_stale_nodeid_prevented", "AC2", ast)

    # AC3
    must("ac2800", "AC3", test)
    must("2800", "AC3", test)
    must("make_ref_layout", "AC3", test)
    must("parent_reverse_edges_ok", "AC3", test)
    must("replace-pattern", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_replace_pattern_multi_match_nodeid_stability.cpp").is_file():
        fails.append("AC3: missing test_replace_pattern_multi_match_nodeid_stability.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2800.cpp").is_file():
        fails.append("AC3: test_issue_2800.cpp present (forbidden per #81967)")
    must("test_replace_pattern_multi_match_nodeid_stability", "AC3", cmake)

    # AC4
    must("check_replace_pattern_multi_match_nodeid_stability_2800", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2800-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2800 replace-pattern multi-match NodeId stability — StableNodeRef two-phase + stale metric")
    return 0


if __name__ == "__main__":
    sys.exit(main())
