#!/usr/bin/env python3
"""Issue #3064: InlinePass refuses MacroIntroduced *body* instructions.

Per-instruction source_marker (already stamped by lowering emit) is
consulted on every inline decision so a User-level IRFunction whose
body still carries MacroIntroduced cannot be treated as ordinary.
Soft/allow (respect_macro_hygiene_ == false) skips the scan.

  AC1 function-level marker still preserved (existing refuse)
  AC2 instruction-level marker present after lowering
  AC3 InlinePass refuses User fn + MacroIntroduced body without allow
  AC4 extend test_jit_macro_introduced_preserve; no invent / docs

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    pass_impl = _read("src/compiler/pass_impls.ixx")
    low = _read("src/compiler/lowering.ixx")
    q = read_query_prims()
    t = _read("tests/compiler/test_jit_macro_introduced_preserve.cpp")
    build = _read("build.py")

    must("Issue #3064", "AC1 cite", pass_impl)
    must("body_has_macro_introduced", "AC1 helper", pass_impl)
    must("3064 AC1", "AC1 test", t)

    must("Issue #3064", "AC2 lowering", low)
    must("source_marker", "AC2 emit", low)
    must("propagate_marker_from_ast", "AC2 propagate", low)
    must("3064 AC2", "AC2 test", t)
    must_key("schema-3064", "AC2 query", q)
    must_key("inline-body-macro-hygiene-wired", "AC2 wired", q)

    must("body_has_macro_introduced(*callee)", "AC3 run_on_block", pass_impl)
    must("body_has_macro_introduced(callee)", "AC3 try_inline", pass_impl)
    must("body_has_macro_introduced(func)", "AC3 predicate", pass_impl)
    must("3064 AC3: User fn + MacroIntroduced body refused", "AC3 test", t)

    must("check_inline_macro_body_marker_3064", "AC4 build", build)
    must("cmd_inline_macro_body_marker_3064", "AC4 cmd", build)
    must("3064 AC4", "AC4 test", t)
    if (ROOT / "tests" / "compiler" / "test_issue_3064.cpp").is_file():
        fails.append("AC4: test_issue_3064.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3064-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")
    if "query:inline-body-macro" in q:
        fails.append("AC4: new top-level query key (forbidden)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3064 InlinePass MacroIntroduced body refuse — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
