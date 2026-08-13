#!/usr/bin/env python3
"""Issue #2961: rename-symbol / replace-pattern Guard + hygiene + restamp.

Contract:
  AC1 Public + lockless: try_acquire / hygiene reject / restamp_all_node_generations
  AC2 Counters rename_symbol_hygiene_reject_total / replace_pattern_hygiene_reject_total
  AC3 Success multi-match restamp; #2800 stale counter preserved
  AC4 Soft green; schema-2961 additive; no docs/design/*
  AC5 Extend test_hygiene_mutate_closed_loop; this linter; build gate

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

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    efl = _read("src/compiler/evaluator_eval_flat.cpp")
    ast = _read("src/core/ast.ixx")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")

    def _prim_window(hay: str, name: str) -> str:
        key = f'add_mutate("{name}"'
        pos = hay.find(key)
        if pos < 0:
            pos = hay.find(name)
            if pos < 0:
                return ""
            return hay[pos : pos + 8000]
        # Full prim body until next add_mutate (replace-pattern is large).
        nxt = hay.find("add_mutate(", pos + len(key))
        end = nxt if nxt > pos else pos + 32000
        return hay[pos:end]

    rwin = _prim_window(mut, "mutate:rename-symbol")
    pwin = _prim_window(mut, "mutate:replace-pattern")

    # AC1
    must("try_acquire", "AC1 rename", rwin)
    must("try_acquire", "AC1 replace-pattern", pwin)
    must("note_rename_symbol_hygiene_reject", "AC1 rename", rwin)
    must("restamp_all_node_generations", "AC1 rename", rwin)
    must("note_replace_pattern_hygiene_reject", "AC1 replace-pattern", pwin)
    must("restamp_all_node_generations", "AC1 replace-pattern", pwin)
    must("#2961", "AC1 mut", mut)
    must("note_rename_symbol_hygiene_reject", "AC1 lockless rename", efl)
    must("note_replace_pattern_hygiene_reject", "AC1 lockless replace", efl)
    must("restamp_all_node_generations", "AC1 lockless restamp", efl)
    must("#2961", "AC1 efl", efl)
    # include-macro must NOT alone authorize mutate (allow_macro_all without include).
    if "allow_macro_all = ev.get_allow_macro_mutate() || allow_macro_kw || include_macro_introduced" in pwin:
        fails.append("AC1: include_macro_introduced must not authorize mutate (use :allow-macro?)")
    must("get_allow_macro_mutate() || allow_macro_kw", "AC1 allow_macro_all", pwin)

    # AC2
    must("rename_symbol_hygiene_reject_total_", "AC2", ast)
    must("replace_pattern_hygiene_reject_total_", "AC2", ast)
    must("note_rename_symbol_hygiene_reject", "AC2", ast)
    must("note_replace_pattern_hygiene_reject", "AC2", ast)

    # AC3
    must("replace_pattern_stale_nodeid_prevented", "AC3", ast)
    must("note_replace_pattern_stale_nodeid_prevented", "AC3", mut)

    # AC4
    must("schema-2961", "AC4", q)
    must("issue-2961", "AC4", q)
    must("rename-symbol-hygiene-reject-total", "AC4", q)
    must("replace-pattern-hygiene-reject-total", "AC4", q)
    must("rename-replace-hygiene-restamp-wired", "AC4", q)
    must("schema-2037", "AC4 lineage", q)

    # AC5
    must("ac2961_1_source_guard_hygiene_restamp", "AC5", test)
    must("#2961", "AC5", test)
    must("check_rename_replace_hygiene_restamp_2961", "AC5", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2961.cpp").is_file():
        fails.append("AC5: test_issue_2961.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("*2961*"):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2961 rename/replace-pattern Guard hygiene restamp — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
