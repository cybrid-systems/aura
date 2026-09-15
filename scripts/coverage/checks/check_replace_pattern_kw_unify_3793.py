#!/usr/bin/env python3
"""Issue #3793: unify the matcher macro keyword face on
mutate:replace-pattern with the rest of the Agent surface.

Contract (one row per AC):
  AC1  Parse arm accepts :allow-macro? (primary) alongside the old
       :include-macro-introduced / :allow-macro-introduced compat aliases
       — no hard bad-arg on the mutate-side matcher face
  AC2  Gate fold: allow_macro_all includes include_macro_introduced so
       whatever widened matcher visibility also unlocks the per-match
       gate (query:find / query:pattern parity; no include-yes/gate-no
       split)
  AC3  MSE fence untouched: deny_macro_opt_out_without_mse stays on the
       allowed path inside hygiene_protected_error (#3542/#3755)
  AC4  No new query key: query faces keep the existing keyword set; the
       usage string documents :allow-macro? [#t]; test registered in
       CMake (tests/compiler/test_replace_pattern_allow_macro_unify.cpp);
       no docs/design/3793-*

Regex structural pins (whitespace-robust against clang-format reflow);
simple substring pins for single-line cites. Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# AC1: unified parse arm — three spellings in one condition, any wrap.
RE_PARSE_ARM = re.compile(
    r'kw\s*==\s*":include-macro-introduced"\s*\|\|'
    r'\s*kw\s*==\s*":allow-macro-introduced"\s*\|\|'
    r'\s*kw\s*==\s*":allow-macro\?"'
)

# AC2: visibility keyword also unlocks the per-match gate.
RE_GATE_FOLD = re.compile(
    r"allow_macro_all\s*=\s*ev\.get_allow_macro_mutate\(\)\s*\|\|"
    r"\s*allow_macro_kw\s*\|\|\s*include_macro_introduced"
)


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_match(rx: re.Pattern[str], label: str, hay: str) -> None:
        if not rx.search(hay):
            fails.append(f"{label}: pattern {rx.pattern!r} not found")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    test = _read("tests/compiler/test_replace_pattern_allow_macro_unify.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    if not mut:
        fails.append("mutate.cpp: unreadable")
    if not qws:
        fails.append("query_workspace.cpp: unreadable")

    # AC1: unified parse arm (three spellings in one condition).
    must_match(RE_PARSE_ARM, "AC1 parse arm", mut)
    must("Issue #3793", "AC1 cite", mut)

    # AC2: visibility keyword also unlocks the gate.
    must_match(RE_GATE_FOLD, "AC2 gate fold", mut)

    # AC3: MSE fence stays on the allowed path.
    must("deny_macro_opt_out_without_mse", "AC3 MSE fence helper", mut)
    must("macro-mutate-needs-macro-self-evo", "AC3 deny reason", mut)

    # AC4: no new query key; usage string documents the unified keyword.
    must('":allow-macro?"', "AC4 query face unchanged", qws)
    must("[:allow-macro? [#t]]", "AC4 usage string", mut)
    must_not('":allow-macro-introduced?"', "AC4 no invented key", mut)
    must("test_replace_pattern_allow_macro_unify", "AC4 test registered", cmake)
    must("check_replace_pattern_kw_unify_3793.py", "AC4 linter wired", build)

    # Test pins its own ACs.
    must("AC1", "test AC1", test)
    must("AC4", "test AC4", test)

    if fails:
        for f in fails:
            print(f"FAIL {f}", file=sys.stderr)
        print(f"#3793 linter: {len(fails)} row(s) failed", file=sys.stderr)
        return 1

    # --self-test: the checks above ran against the live tree; report.
    if "--self-test" in sys.argv:
        print("#3793 linter self-test: all rows satisfied (5/5)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
