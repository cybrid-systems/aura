#!/usr/bin/env python3
"""Issue #2797: replace-subtree hygiene-checks new subtree for MacroIntroduced.

Target-only MacroIntroduced gate left a hole: install macro-provenance body
under a normal parent (same class as #2792 rebind).

Contract (one row per AC):
  AC1 public + lockless cite #2797; walk_subtree(pr.root) + is_macro_introduced
  AC2 free orphans on new-body hygiene reject (both paths)
  AC3 tests/compiler/test_replace_subtree_new_body_hygiene.cpp + no test_issue_2797.cpp
  AC4 this linter wired; no docs/design/2797-*

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
    test = _read("tests/compiler/test_replace_subtree_new_body_hygiene.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    pos = mut.find('add_mutate("mutate:replace-subtree"')
    if pos < 0:
        pos = mut.find("mutate:replace-subtree")
    pwin = mut[pos : pos + 12000] if pos >= 0 else ""

    lpos = flat.find("eval_flat_apply_mutate_replace_subtree")
    lwin = flat[lpos : lpos + 5000] if lpos >= 0 else ""

    # AC1
    must("Issue #2797", "AC1", pwin)
    must("walk_subtree(pr.root", "AC1", pwin)
    must("is_macro_introduced", "AC1", pwin)
    must("Issue #2797", "AC1", lwin)
    must("walk_subtree(pr.root", "AC1", lwin)
    must("is_macro_introduced", "AC1", lwin)

    # AC2
    if "free_replace_parse_orphans" not in pwin and "free_orphan_nodes_from" not in pwin:
        fails.append("AC2: public path missing free orphans on hygiene reject")
    must("free_orphan_nodes_from", "AC2", lwin)

    # AC3
    must("ac2797", "AC3", test)
    must("2797", "AC3", test)
    must("walk_subtree", "AC3", test)
    must("replace-subtree", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_replace_subtree_new_body_hygiene.cpp").is_file():
        fails.append("AC3: missing test_replace_subtree_new_body_hygiene.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2797.cpp").is_file():
        fails.append("AC3: test_issue_2797.cpp present (forbidden per #81967)")
    must("test_replace_subtree_new_body_hygiene", "AC3", cmake)

    # AC4
    must("check_replace_subtree_new_body_hygiene_2797", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2797-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2797 replace-subtree new-body hygiene — walk_subtree(pr.root) + MacroIntroduced reject")
    return 0


if __name__ == "__main__":
    sys.exit(main())
