#!/usr/bin/env python3
"""
migrate_to_harness.py — migrate all test_issue_*.cpp files to use
tests/test_harness.hpp.

Idempotent: re-running on already-migrated files is a no-op.

For each file that matches the standard pattern:
  static int g_passed = 0;
  static int g_failed = 0;
  #define CHECK(cond, msg) ...

  int main() {
      ... test calls ...
      std::println("Results: {} passed, {} failed", g_passed, g_failed);
      return g_failed == 0 ? 0 : 1;
  }

The script:
  1. Adds `#include "test_harness.hpp"` after the existing #include
     block (or after the import block, whichever comes last)
  2. Removes the local `static int g_passed = 0; static int g_failed = 0;`
     and the local `CHECK` macro definition
  3. Replaces the `std::println("Results: ..."); return g_failed == 0 ? 0 : 1;`
     at the end of main() with `return RUN_ALL_TESTS();`

Skips files that:
  - Don't have the g_passed/g_failed pattern (e.g., test_issue_159_bench.cpp)
  - Are already migrated (have `RUN_ALL_TESTS()`)
  - Are in a known-skip list (e.g., benchmarks, standalone tests)

Usage:
  python3 tests/migrate_to_harness.py            # migrate all
  python3 tests/migrate_to_harness.py --dry-run  # show what would change
  python3 tests/migrate_to_harness.py --only 196 # migrate only test_issue_196.cpp
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"

# Files to skip entirely
SKIP_FILES = {
    "test_issue_159_bench.cpp",  # benchmark, not a test
    "test_issue_224.cpp",  # already migrated
}

# Pattern: `static int g_passed = 0;` (or `g_passed = 0;`)
G_PASSED_RE = re.compile(r"^static\s+int\s+g_passed\s*=\s*0\s*;\s*$", re.MULTILINE)
G_FAILED_RE = re.compile(r"^static\s+int\s+g_failed\s*=\s*0\s*;\s*$", re.MULTILINE)

# Pattern: the local CHECK macro (matches both backslash-continuation and
# single-line forms; we look for `#define CHECK(` and consume through the
# matching `} while (0)` or end of statement).
CHECK_MACRO_RE = re.compile(
    r"^#define\s+CHECK\s*\([^)]*\)\s+do\s*\{.*?\}\s*while\s*\(\s*0\s*\)\s*\n",
    re.MULTILINE | re.DOTALL,
)

# Pattern: the main() summary + return at the end
MAIN_SUMMARY_RE = re.compile(
    r"std::println\(\"Results: \{\} passed, \{\} failed\",\s*g_passed,\s*g_failed\)\s*;\s*"
    r"return\s+g_failed\s*==\s*0\s*\?\s*0\s*:\s*1\s*;",
    re.MULTILINE,
)


def needs_migration(content: str) -> bool:
    if "RUN_ALL_TESTS" in content:
        return False
    if not G_PASSED_RE.search(content):
        return False
    return G_FAILED_RE.search(content)


def migrate(content: str) -> tuple[str, bool]:
    """Returns (new_content, changed)."""
    if not needs_migration(content):
        return content, False

    new = content

    # 1. Remove the local CHECK macro
    new = CHECK_MACRO_RE.sub("", new)

    # 2. Remove the local g_passed / g_failed declarations
    new = G_PASSED_RE.sub("", new)
    new = G_FAILED_RE.sub("", new)

    # 3. Replace the main() summary + return with RUN_ALL_TESTS()
    new = MAIN_SUMMARY_RE.sub("return RUN_ALL_TESTS();", new)

    # 4. Add `#include "test_harness.hpp"` after the last #include
    #    (or after the import block if no #include follows).
    if '#include "test_harness.hpp"' not in new:
        # Find the last #include line
        last_include_idx = -1
        for m in re.finditer(r"^#include[^\n]*\n", new, re.MULTILINE):
            last_include_idx = m.end()
        if last_include_idx > 0:
            new = (
                new[:last_include_idx]
                + "\n// Unified test harness (Issue #226). Provides\n"
                + "// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local\n"
                + "// g_passed / g_failed / CHECK macro above are removed;\n"
                + "// this file now uses the harness's versions.\n"
                + '#include "test_harness.hpp"\n'
                + new[last_include_idx:]
            )
        else:
            # No #include — insert before the first import
            first_import = re.search(r"^import\s+", new, re.MULTILINE)
            if first_import:
                new = (
                    new[: first_import.start()]
                    + "// Unified test harness (Issue #226).\n"
                    + '#include "test_harness.hpp"\n'
                    + "\n"
                    + new[first_import.start() :]
                )
            else:
                # Plain file — insert at top
                new = "// Unified test harness (Issue #226).\n" + '#include "test_harness.hpp"\n' + "\n" + new

    # 5. If g_passed / g_failed are still referenced in the
    #    file (e.g., a custom summary format in main() like
    #    `g_passed, g_failed`), add a `using` declaration so
    #    they resolve to the harness's globals. Must run
    #    AFTER the #include insertion so the replace() can
    #    find the include line.
    if ("g_passed" in new or "g_failed" in new) and "using aura::test::g_passed" not in new:
        new = new.replace(
            '#include "test_harness.hpp"',
            '#include "test_harness.hpp"\nusing aura::test::g_passed;\nusing aura::test::g_failed;',
            1,
        )

    return new, True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true", help="don't write changes")
    ap.add_argument("--only", default=None, help="only migrate this filename (e.g. 196)")
    args = ap.parse_args()

    files = sorted(TESTS.glob("test_issue_*.cpp"))
    if args.only:
        files = [f for f in files if args.only in f.name]

    total = 0
    changed = 0
    skipped = 0
    errors = []
    for f in files:
        if f.name in SKIP_FILES:
            skipped += 1
            print(f"  ⊘ {f.name} (skip list)")
            continue
        total += 1
        original = f.read_text()
        new_content, did_change = migrate(original)
        if not did_change:
            if "RUN_ALL_TESTS" in new_content:
                print(f"  ✓ {f.name} (already migrated)")
            elif G_PASSED_RE.search(new_content):
                print(f"  ? {f.name} (matches pattern but no changes? investigate)")
            else:
                print(f"  - {f.name} (no g_passed/g_failed pattern)")
            continue
        if args.dry_run:
            print(f"  → {f.name} (would change)")
        else:
            f.write_text(new_content)
            print(f"  ✓ {f.name} (migrated)")
        changed += 1
    print(f"\nSummary: {total} files, {changed} changed, {skipped} skipped")
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
