#!/usr/bin/env python3
"""Issue #2798: replace-pattern frees parse orphans on skip / zero-replace.

rollback_atomic_batch does not free SoA nodes; per-iteration parse_to_flat
appends must free_orphan_nodes_from on skip and when replaced_count==0.

Contract (one row per AC):
  AC1 lockless + public cite #2798; free_orphan on skip + zero-replace
  AC2 free_repl_parse_orphans / size_before_parse bound
  AC3 tests/compiler/test_replace_pattern_no_match_no_leak.cpp + no test_issue_2798.cpp
  AC4 this linter wired; no docs/design/2798-*

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
    test = _read("tests/compiler/test_replace_pattern_no_match_no_leak.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    lpos = flat.find("eval_flat_apply_mutate_replace_pattern")
    # Body through zero-replace free (~6.5KB+)
    lwin = flat[lpos : lpos + 9000] if lpos >= 0 else ""

    ppos = mut.find('add_mutate("mutate:replace-pattern"')
    if ppos < 0:
        ppos = mut.find("mutate:replace-pattern")
    # Public body is long (QueryMatcher + apply loop); #2798 near end ~20KB.
    pwin = mut[ppos : ppos + 24000] if ppos >= 0 else ""

    # AC1
    must("Issue #2798", "AC1", lwin)
    must("free_orphan_nodes_from", "AC1", lwin)
    must("replaced_count == 0", "AC1", lwin)
    must("Issue #2798", "AC1", pwin)
    must("free_orphan_nodes_from", "AC1", pwin)

    # AC2
    must("free_repl_parse_orphans", "AC2", lwin)
    must("size_before_parse", "AC2", lwin)
    must("free_repl_parse_orphans", "AC2", pwin)

    # AC3
    must("ac2798", "AC3", test)
    must("2798", "AC3", test)
    must("live_node_count", "AC3", test)
    must("replace-pattern", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_replace_pattern_no_match_no_leak.cpp").is_file():
        fails.append("AC3: missing test_replace_pattern_no_match_no_leak.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2798.cpp").is_file():
        fails.append("AC3: test_issue_2798.cpp present (forbidden per #81967)")
    must("test_replace_pattern_no_match_no_leak", "AC3", cmake)

    # AC4
    must("check_replace_pattern_no_match_no_leak_2798", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2798-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2798 replace-pattern no-match no leak — free_orphan_nodes_from on skip + zero-replace")
    return 0


if __name__ == "__main__":
    sys.exit(main())
