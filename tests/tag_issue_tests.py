#!/usr/bin/env python3
"""
tag_issue_tests.py — Categorize each test_issue_*.cpp into one of:

  - unit         Pure C++ unit test (no CompilerService, no Aura eval)
  - integration  Runs Aura programs end-to-end via CompilerService
  - regression   Tests a specific bug fix (would otherwise have been
                 a real bug; guards against re-introducing it)
  - issue_specific  Tests verifying a specific issue's feature
                 (default for test_issue_*.cpp)

Heuristics (best-effort; review output and adjust manually):

  1. If file does NOT import aura.compiler.service AND does NOT
     construct a CompilerService → unit
  2. If file imports aura.compiler.service AND uses a
     CompilerService to eval Aura source → integration
  3. If file has 'Regression' or 'regression' in its top-level
     comment block → regression
  4. Otherwise → issue_specific

The tag is added as a structured comment near the top of the
file:
  // @category: unit
  // @reason: <one-line reason>

This is the source of truth for the CMake refactor (cycle 5)
and the test migration (cycle 6).

Usage:
  python3 tests/tag_issue_tests.py            # tag all + print report
  python3 tests/tag_issue_tests.py --only 196 # tag only #196
  python3 tests/tag_issue_tests.py --dry-run  # show report, don't write
"""

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"

# Tag patterns. Each tag is added as a structured comment
# line. The first occurrence is replaced; subsequent ones
# are ignored.
CATEGORY_LINE_RE = re.compile(r"^//\s*@category:\s*\w+\s*$", re.MULTILINE)
REASON_LINE_RE = re.compile(r"^//\s*@reason:.*$", re.MULTILINE)


def has_compiler_service(content: str) -> bool:
    """True if the file imports or uses aura::compiler::CompilerService."""
    if "aura.compiler.service" in content or "CompilerService" in content:
        return True
    return False


def evals_aura_source(content: str) -> bool:
    """True if the file uses a CompilerService to eval Aura source.

    Looks for patterns like `cs.eval(...)` or `run_on(cs, ...)`
    or similar.
    """
    if re.search(r"\bcs\.eval\s*\(", content):
        return True
    if re.search(r"\brun_on\s*\(\s*cs\s*,", content):
        return True
    if re.search(r"CompilerService\s+cs[;\s]", content):
        return True
    return False


def has_regression_keyword(content: str, filename: str) -> bool:
    """True if the file's top comment block mentions regression.

    A regression test guards against a specific bug fix being
    undone. The convention is to mention 'regression' in the
    top-level comment.
    """
    # Look at the first 30 lines (the top-of-file comment block).
    head = "\n".join(content.splitlines()[:30]).lower()
    return "regression" in head or "regression for" in head


def categorize(content: str, filename: str) -> tuple[str, str]:
    """Return (category, reason) for the file."""
    is_unit = not has_compiler_service(content)
    is_integration = has_compiler_service(content) and evals_aura_source(content)
    is_regression = has_regression_keyword(content, filename)
    if is_unit and not has_compiler_service(content):
        # Verify: no CompilerService references at all
        if "CompilerService" not in content:
            return ("unit", "no CompilerService usage; pure C++ test")
    if is_integration:
        return ("integration", "uses CompilerService to eval Aura source")
    if is_regression:
        return ("regression", "mentions 'regression' in top comment")
    return ("issue_specific", "default for test_issue_*.cpp")


def add_tags(content: str, category: str, reason: str) -> str:
    """Add or update the @category / @reason header tags."""
    # Remove any existing @category / @reason lines
    new = CATEGORY_LINE_RE.sub("", content)
    new = REASON_LINE_RE.sub("", new)
    # Insert the new tags at the top of the file
    tag_block = f"// @category: {category}\n// @reason: {reason}\n"
    new = tag_block + new
    return new


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true", help="don't write changes")
    ap.add_argument("--only", default=None, help="only tag this filename (e.g. 196)")
    args = ap.parse_args()

    files = sorted(TESTS.glob("test_issue_*.cpp"))
    if args.only:
        files = [f for f in files if args.only in f.name]

    counts = Counter()
    assignments = []
    for f in files:
        content = f.read_text()
        category, reason = categorize(content, f.name)
        counts[category] += 1
        assignments.append((f.name, category, reason))
        if not args.dry_run:
            new = add_tags(content, category, reason)
            f.write_text(new)

    # Print the report
    print(f"\n=== Tagging report ({len(assignments)} files) ===\n")
    for cat in ("unit", "integration", "regression", "issue_specific"):
        n = counts.get(cat, 0)
        print(f"  {cat}: {n} files")
    print()
    for cat in ("unit", "integration", "regression", "issue_specific"):
        files_in_cat = [a for a in assignments if a[1] == cat]
        if not files_in_cat:
            continue
        print(f"--- {cat} ({len(files_in_cat)} files) ---")
        for name, _, reason in files_in_cat:
            print(f"  {name}: {reason}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
