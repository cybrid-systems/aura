#!/usr/bin/env python3
# scripts/check_nested_qq_depth_3606.py -- Issue #3606 source-cite gate.
#
# Verifies nested quasiquote depth semantics (#2807/#2239 residual):
#
#  AC1: pre_scan unquote / unquote-splicing at depth > 1 recurses with
#       qq_depth - 1 (outer-template scope); at depth <= 1 stops
#       (caller scope) and the splicing mismatch counter keeps firing
#       only there (AC6). The clone walk threads int qq_depth (replaces
#       the #2807 bool in_unquote); the caller-scope verbatim zone is
#       the negative sentinel kUnquoteCallerScopeDepth, per-child depth
#       assigned at the recursion site (quasiquote arg +1, unquote arg
#       -1 / depth-1).
#  AC2/AC3: test face — test_unquote_splicing_hygiene.cpp drives the
#       nested-qq reproducer (double-qq + inner let + inner unquote →
#       binding AND operand renamed; splice at depth 2 template scope;
#       single-level splice keeps caller scope).
#  AC4: Soft path — name_map-less clone still covered (same walk).
#  AC5: no docs/design/3606-* (#1655); no tests/**/test_issue_3606.cpp
#       (#81934); build.py wires this linter.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

ME = "src/compiler/macro_expansion.cpp"
TEST = "tests/compiler/test_unquote_splicing_hygiene.cpp"
BUILD = "build.py"

REQUIRED: tuple[tuple[str, str, str], ...] = (
    # AC1: pre_scan depth rules.
    (ME, r"Issue\s+#3606", "3606 AC1: macro_expansion.cpp cites #3606"),
    (
        ME,
        r"if \(qq_depth > 1 && nv\.children\.size\(\) >= 2\)",
        "3606 AC1: pre_scan unquote recurses at depth > 1",
    ),
    (
        ME,
        r"kUnquoteCallerScopeDepth",
        "3606 AC1: caller-scope verbatim sentinel defined",
    ),
    (
        ME,
        r"bool in_quote = false, int qq_depth = 0\);",
        "3606 AC1: clone signature threads int qq_depth (fwd decl)",
    ),
    (
        ME,
        r"const bool local_in_unquote = qq_depth < 0;",
        "3606 AC1: verbatim zone derived from the negative sentinel",
    ),
    (
        ME,
        r"int child_qq_depth = qq_depth;",
        "3606 AC1: per-child depth assigned at the recursion site",
    ),
    (
        ME,
        r"g_unquote_splicing_hygiene_mismatch_total",
        "3606 AC6: splicing mismatch counter retained",
    ),
    # AC2/AC3: test face.
    (TEST, r"#3606", "3606 AC2: splicing test hosts #3606 ACs"),
    (
        TEST,
        r"build_nested_qq_let_unquote",
        "3606 AC2: nested-qq unquote reproducer builder",
    ),
    (
        TEST,
        r"build_nested_qq_let_splicing",
        "3606 AC3: nested-qq splicing builder",
    ),
    (BUILD, r"check_nested_qq_depth_3606", "3606 AC5: build.py wires the linter"),
)

FORBIDDEN: tuple[tuple[str, str, str], ...] = (
    (
        ME,
        r"bool in_unquote",
        "3606 AC1: boolean in_unquote must be replaced by int qq_depth",
    ),
)


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def run_checks() -> list[str]:
    """Returns a list of failure labels (empty = clean)."""
    failures: list[str] = []
    cache: dict[str, str] = {}

    def body(path: str) -> str:
        if path not in cache:
            full = REPO_ROOT / path
            cache[path] = read(full) if full.exists() else ""
        return cache[path]

    for path, pattern, label in REQUIRED:
        if re.search(pattern, body(path)) is None:
            failures.append(label)

    for path, pattern, label in FORBIDDEN:
        if re.search(pattern, body(path)) is not None:
            failures.append(label)

    flat = body(ME)

    # AC1: both unquote and unquote-splicing recurse with depth - 1.
    if flat.count("pre_scan(nv.child(1), qq_depth - 1);") < 2:
        failures.append("3606 AC1: expected >=2 depth-1 recursions (unquote + unquote-splicing)")

    # AC6: the splicing counter fires only under the depth <= 1 gate.
    pos = flat.find('if (cname == "unquote-splicing") {')
    if pos < 0:
        failures.append("3606 AC6: splicing handler not found")
    else:
        window = flat[pos : pos + 600]
        if "qq_depth <= 1" not in window:
            failures.append("3606 AC6: counter not gated on depth <= 1")
        if "fetch_add" not in window:
            failures.append("3606 AC6: counter bump missing from the depth-1 branch")
        if "pre_scan(nv.child(1), qq_depth - 1);" not in window:
            failures.append("3606 AC1: depth > 1 splice body recursion missing")

    # AC1: per-child depth — quasiquote arg +1, unquote arg sentinel /
    # depth-1. The assignment block must precede the recursive call.
    child_pos = flat.find("int child_qq_depth = qq_depth;")
    call_pos = flat.find("hygiene_depth + 1, depth_limit, local_in_quote, child_qq_depth);")
    if child_pos < 0 or call_pos < 0 or not (child_pos < call_pos):
        failures.append("3606 AC1: per-child depth must precede the recursive clone call")
    else:
        window = flat[child_pos:call_pos]
        for needle in (
            '"quasiquote"',
            "qq_depth + 1",
            "qq_depth <= 1",
            "qq_depth - 1",
        ):
            if needle not in window:
                failures.append(f"3606 AC1: child depth rule missing: {needle}")

    # AC5: no design doc, no standalone issue test (#1655 / #81934).
    design = REPO_ROOT / "docs" / "design"
    if design.is_dir():
        for entry in design.iterdir():
            if entry.name.startswith("3606-"):
                failures.append(f"3606 AC5: docs/design/{entry.name} must not exist (#1655)")
    for probe in (
        REPO_ROOT / "tests" / "core" / "test_issue_3606.cpp",
        REPO_ROOT / "tests" / "issues" / "test_issue_3606.cpp",
        REPO_ROOT / "tests" / "compiler" / "test_issue_3606.cpp",
    ):
        if probe.exists():
            failures.append(f"3606 AC5: {probe.relative_to(REPO_ROOT)} must not exist (#81934)")

    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3606 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="fail on any missing row")
    ap.parse_args()
    failures = run_checks()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"check_nested_qq_depth_3606: {len(failures)} failure(s)")
        return 1
    print("check_nested_qq_depth_3606: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
