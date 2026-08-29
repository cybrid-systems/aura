#!/usr/bin/env python3
"""Issue #3354: query:pattern / query:find default-skip MacroIntroduced.

Mutate structural hotpaths already default-reject MacroIntroduced unless
:allow-macro? (#2037 / #3191 / #3344). Query was visibility-first — Agents
could plan mutate targets that production mutate then rejects.

Contract:
  AC1  production_defaults + query:pattern / find without :allow-macro?
       skip MacroIntroduced (shared query_hygiene_allow_macro helper)
  AC2  :allow-macro? unlocks (same face as mutate)
  AC3  Soft / Off: find keeps today's include (no forced skip)
  AC4  source-cite shared helper with reject_structural_macro_hygiene;
       extend test_query_pattern_default_hygiene
  AC5  linter AFTER #3344; no docs/design/; no test_issue_3354.cpp;
       no schema-3354 / g_3354_*

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

    qws = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    matcher = _read("src/compiler/query_matcher.ixx")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    t = _read("tests/compiler/test_query_pattern_default_hygiene.cpp")
    build = _read("build.py")

    # AC1 — production skip + shared helper.
    must("query_hygiene_allow_macro", "AC1 helper", qws)
    must("kQueryPatternFindHygieneAlignIssue = 3354", "AC1 stamp", matcher)
    must(":allow-macro?", "AC1 mutate keyword", qws)
    must("production_defaults_active()", "AC1 find gate", qws)
    must("skip_macro && flat.is_macro_introduced(id)", "AC1 find skip", qws)
    must(
        "include_macro_introduced = query_hygiene_allow_macro",
        "AC1 pattern helper",
        qws,
    )
    must("bump_macro_introduced_skipped_in_query", "AC1 existing skip counter", qws)

    # AC2 — :allow-macro? unlock on pattern / find.
    must('kw == ":allow-macro?"', "AC2 pattern keyword", qws)
    must('kw == ":allow-macro?" || kw == ":include-macro-introduced"', "AC2 find keyword", qws)
    must("ac3354_2_allow_macro_unlock", "AC2 test", t)

    # AC3 — Soft find include.
    must("Soft / Off: no skip (today's include)", "AC3 Soft find", qws)
    must("ac3354_3_soft_find_include", "AC3 test", t)

    # AC4 — shared face with mutate gate.
    must("reject_structural_macro_hygiene", "AC4 query cite", qws)
    must("reject_structural_macro_hygiene", "AC4 mutate gate", mut)
    must("ac3354_4_match_subset_mutate", "AC4 test", t)
    must("ac3354_1_production_find_skip", "AC4 AC1 test", t)

    # AC5 — linter after #3344; no invent.
    must("check_query_pattern_find_hygiene_align_3354", "AC5 build.py", build)
    must("ac3354_5_source_and_linter", "AC5 test", t)
    prev = build.find("check_mutate_hygiene_continuous_gate_3344")
    ours = build.find("check_query_pattern_find_hygiene_align_3354")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3344")
    if "schema-3354" in qws or "schema-3354" in matcher:
        fails.append("AC5: new schema-3354 query key")
    if "g_3354_" in qws or "g_3354_" in matcher:
        fails.append("AC5: new g_3354_* counter")
    if _read("tests/compiler/test_issue_3354.cpp"):
        fails.append("AC5: test_issue_3354.cpp present (forbidden #81967)")
    if _read("tests/issues/test_issue_3354.cpp"):
        fails.append("AC5: tests/issues/test_issue_3354.cpp present (forbidden #81967)")
    if _read("docs/design/3354-query-pattern-find-hygiene.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3354 query_pattern_find_hygiene_align:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3354 query_pattern_find_hygiene_align: pattern/find default-skip MacroIntroduced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
