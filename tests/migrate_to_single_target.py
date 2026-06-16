#!/usr/bin/env python3
"""
migrate_to_single_target.py — Refactor test_issue_*.cpp from
per-file binary to a single combined binary.

This is the Issue #226 cycle 3 refactor: instead of 85
separate ninja targets (one per test_issue_*.cpp), we
build ONE target `test_issues` that links all the test
sources together with a wrapper main.

Steps:
  1. In each test_issue_NNN.cpp, rename `int main()` to
     `int run_issue_NNN()`. The function still does what
     main() did — runs the test functions and returns
     the result of RUN_ALL_TESTS().
  2. The wrapper `tests/test_issues_main.cpp` has the
     real `int main()` that calls every `run_issue_NNN()`
     in sequence, aggregating pass/fail counts.
  3. CMakeLists.txt: replace 85 `aura_add_issue_test(...)`
     calls with a single `aura_add_issues_target()` that
     builds one binary.

Idempotent: re-running on already-renamed files is a
no-op (the file already has `int run_issue_NNN` instead
of `int main`).

Usage:
  python3 tests/migrate_to_single_target.py            # rename mains
  python3 tests/migrate_to_single_target.py --write-wrapper  # also write the wrapper
  python3 tests/migrate_to_single_target.py --dry-run  # show what would change
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"

# Match `int main() {` exactly (no args). The tests don't
# use `int main(int argc, char** argv)` so this is safe.
MAIN_RE = re.compile(r"^int\s+main\s*\(\s*\)\s*\{", re.MULTILINE)

# Files that are NOT individual test_issue binaries
# (e.g., benchmarks). These don't have a main to rename.
SKIP_FILES = {
    "test_issue_159_bench.cpp",   # benchmark, no main pattern
}


def extract_issue_number(filename: str) -> str:
    """Extract NNN from test_issue_NNN.cpp."""
    m = re.match(r"test_issue_(\d+)\.cpp$", filename)
    if not m:
        return ""
    return m.group(1)


def rename_main_in_file(content: str, nnn: str) -> tuple[str, bool]:
    """Rename `int main()` to `int run_issue_NNN()` in the content.
    Returns (new_content, changed).
    """
    new_func = f"int run_issue_{nnn}() {{"
    if new_func in content:
        # Already renamed
        return content, False
    new, count = MAIN_RE.subn(new_func, content)
    if count == 0:
        return content, False
    return new, True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true", help="don't write changes")
    args = ap.parse_args()

    files = sorted(TESTS.glob("test_issue_*.cpp"))
    total = 0
    changed = 0
    skipped = 0
    for f in files:
        if f.name in SKIP_FILES:
            skipped += 1
            print(f"  ⊘ {f.name} (skip list)")
            continue
        nnn = extract_issue_number(f.name)
        if not nnn:
            print(f"  ? {f.name} (no issue number)")
            continue
        total += 1
        original = f.read_text()
        new_content, did_change = rename_main_in_file(original, nnn)
        if not did_change:
            print(f"  - {f.name} (no main or already renamed)")
            continue
        if args.dry_run:
            print(f"  → {f.name} (would rename main → run_issue_{nnn})")
        else:
            f.write_text(new_content)
            print(f"  ✓ {f.name} (renamed main → run_issue_{nnn})")
        changed += 1
    print(f"\nSummary: {total} files, {changed} renamed, {skipped} skipped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
