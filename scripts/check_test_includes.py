#!/usr/bin/env python3
"""Issue #1484: include-path sanity gate for tests/.

Prevents bare `#include "<header>.h"` patterns in tests/*.cpp where
the header lives under `src/compiler/` or `src/core/` but the include
doesn't use the `<subdir>/<header>.h` prefix. Discovered during
#1459 close-verify — 9 test files had already shipped broken
includes via `--no-verify` or never-rebuilt (headers had moved from
`<src>/` to `<src>/compiler/` and `<src>/core/`, and the test files
weren't updated). The 9 files were fixed at commit 313c530d; this
linter prevents future regressions.

Detection rules:
  - Lints `tests/*.cpp` (skips `tests/bench/*.cpp`, `tests/build/**`,
    and any path under `.gitignore`).
  - Scans `#include "<header>"` patterns (quoted, not angle-bracket).
    Angle-bracket includes (`<header>`) are system headers and ignored.
  - For each quoted include:
    - If `<header>` is `<header>.h` and the file exists at
      `src/compiler/<header>` OR `src/core/<header>`:
      the include MUST use the `compiler/` or `core/` prefix.
    - Same for `<header>.hh` → must use `compiler/` or `core/` prefix.
    - System-prefixed includes (`compiler/...`, `core/...`,
      `lib/...`, `tests/...`, `aura/...`, etc.) are always OK.
    - Sibling-include (tests/foo.cpp → tests/foo_helper.h) is OK.
    - Unresolvable includes (header not found anywhere under src/) are
      OK — out of scope for this linter (covered by compiler errors).

Usage:
  python3 scripts/check_test_includes.py           # check (CI / gate)
  python3 scripts/check_test_includes.py --self-test  # verify linter
                                                  # with 4 known-broken
                                                  # patterns as fixtures

Exit 0 = OK, 1 = broken-include violation found (or self-test fail).
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = ROOT / "tests"
SRC_COMPILER = ROOT / "src" / "compiler"
SRC_CORE = ROOT / "src" / "core"

# Quoted-include pattern: `#include "<header>"`
# (excludes angle-bracket system includes per the rules above).
QUOTED_INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')

# Path-prefix allowlist: includes that start with any of these prefixes
# are always OK (system-prefixed, not subject to the subdir rule).
ALLOWED_PREFIXES = (
    "compiler/",
    "core/",
    "lib/",
    "tests/",
    "aura/",
    "src/",
    "third_party/",
    "external/",
)


def find_header_subdir(header: str) -> str | None:
    """Return "compiler" / "core" if `header` exists under src/<subdir>/,
    else None. Only checks .h and .hh extensions."""
    for ext in ("", ".h", ".hh"):
        for subdir in (SRC_COMPILER, SRC_CORE):
            candidate = subdir / (header + ext)
            if candidate.is_file():
                return subdir.name  # "compiler" or "core"
    return None


def lint_file(path: Path) -> list[tuple[int, str, str]]:
    """Return list of (line_no, line, reason) violations in `path`."""
    violations: list[tuple[int, str, str]] = []
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return violations

    for lineno, line in enumerate(text.splitlines(), start=1):
        m = QUOTED_INCLUDE_RE.match(line)
        if not m:
            continue
        header = m.group(1)
        # Skip allowed prefixes (system-prefixed includes).
        if any(header.startswith(p) for p in ALLOWED_PREFIXES):
            continue
        # Sibling-include (e.g. tests/foo.cpp → tests/foo_helper.h)
        # is OK because tests/ is in ALLOWED_PREFIXES above.
        # Check if the bare header lives under src/compiler/ or src/core/.
        subdir = find_header_subdir(header)
        if subdir is None:
            continue  # not in our subdir scope — out of scope for linter
        violations.append(
            (
                lineno,
                line.strip(),
                f'bare include "{header}" must be prefixed with "{subdir}/" (header lives at src/{subdir}/{header})',
            )
        )
    return violations


def lint_tests() -> tuple[int, list[tuple[Path, int, str, str]]]:
    """Lint all tests/*.cpp; return (count, violations)."""
    count = 0
    violations: list[tuple[Path, int, str, str]] = []
    for path in sorted(TESTS_DIR.rglob("*.cpp")):
        # Skip build outputs + benches (bench is allowed bare-include
        # because it may legitimately pull in <header>.h outside the
        # standard subdir scope; bench files are not in the dep-scan
        # critical path either).
        rel = path.relative_to(ROOT)
        if "build" in rel.parts or rel.parts[0] == "tests" and "bench" in rel.parts:
            continue
        count += 1
        for lineno, line, reason in lint_file(path):
            violations.append((path, lineno, line, reason))
    return count, violations


def self_test() -> int:
    """Run the linter against 4 known-broken patterns; verify all 4 are
    detected. Exit 0 on success, 1 on any detection failure."""
    fixtures = [
        ("shape.h", "compiler"),  # bare shape.h
        ("shape_profiler.h", "compiler"),  # bare shape_profiler.h
        ("cpp26_contract_stats.h", "core"),  # bare cpp26_contract_stats.h
        ("gap_buffer.hh", "core"),  # bare gap_buffer.hh
    ]
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        # Synthesize the 4 broken-include fixtures in a fake tests/
        # directory. We point the linter at the temp dir via monkey-
        # patching the module-level paths.
        fake_tests = tmpdir / "tests"
        fake_src = tmpdir / "src"
        (fake_tests / "compiler").mkdir(parents=True)
        (fake_tests / "core").mkdir(parents=True)
        (fake_src / "compiler").mkdir(parents=True)
        (fake_src / "core").mkdir(parents=True)

        # Create the headers at the correct subdirs so find_header_subdir
        # can resolve them.
        (fake_src / "compiler" / "shape.h").write_text("// fixture\n")
        (fake_src / "compiler" / "shape_profiler.h").write_text("// fixture\n")
        (fake_src / "core" / "cpp26_contract_stats.h").write_text("// fixture\n")
        (fake_src / "core" / "gap_buffer.hh").write_text("// fixture\n")

        # Create 4 test files with broken includes.
        test_files = [
            ("test_broken_shape.cpp", '#include "shape.h"\n'),
            ("test_broken_shape_profiler.cpp", '#include "shape_profiler.h"\n'),
            ("test_broken_cpp26.cpp", '#include "cpp26_contract_stats.h"\n'),
            ("test_broken_gap_buffer.cpp", '#include "gap_buffer.hh"\n'),
        ]
        for name, content in test_files:
            (fake_tests / name).write_text(content)

        # Monkey-patch the module-level paths for the linter.
        global ROOT, TESTS_DIR, SRC_COMPILER, SRC_CORE
        orig_root, orig_tests, orig_comp, orig_core = (ROOT, TESTS_DIR, SRC_COMPILER, SRC_CORE)
        try:
            ROOT = tmpdir
            TESTS_DIR = fake_tests
            SRC_COMPILER = fake_src / "compiler"
            SRC_CORE = fake_src / "core"

            count, violations = lint_tests()
        finally:
            ROOT, TESTS_DIR, SRC_COMPILER, SRC_CORE = (orig_root, orig_tests, orig_comp, orig_core)

        # We expect exactly 4 violations (one per fixture). The order
        # depends on filesystem iteration (sorted rglob), which may
        # not match the fixture list order — so we use a set-based
        # check that verifies each (header, subdir) pair was seen
        # at least once across the violations list.
        if len(violations) != len(fixtures):
            print(f"FAIL: expected {len(fixtures)} violations, got {len(violations)}", file=sys.stderr)
            for path, lineno, _line, reason in violations:
                print(f"  {path}:{lineno}: {reason}", file=sys.stderr)
            return 1

        seen = set()
        for _path, _lineno, _line, reason in violations:
            for expected_header, expected_subdir in fixtures:
                if expected_header in reason and expected_subdir in reason and expected_header not in seen:
                    seen.add(expected_header)

        if len(seen) != len(fixtures):
            missing = [h for h, _ in fixtures if h not in seen]
            print(f"FAIL: missing fixtures (linter did not detect): {missing}", file=sys.stderr)
            for path, lineno, _line, reason in violations:
                print(f"  {path}:{lineno}: {reason}", file=sys.stderr)
            return 1

        print(
            f"OK: self-test passed — {count} test files scanned, "
            f"{len(violations)} violations detected "
            f"(all 4 known-broken patterns matched)"
        )
        return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Include-path sanity gate for tests/ (Issue #1484)",
    )
    ap.add_argument("--self-test", action="store_true", help="verify linter with 4 known-broken patterns")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    count, violations = lint_tests()
    if violations:
        print(f"FAIL: {len(violations)} broken include(s) in tests/:", file=sys.stderr)
        for path, lineno, line, reason in violations:
            rel = path.relative_to(ROOT)
            print(f"  {rel}:{lineno}: {reason}", file=sys.stderr)
            print(f"    {line}", file=sys.stderr)
        print("", file=sys.stderr)
        print(
            "Fix: prefix the include with the subdir, e.g. "
            '#include "compiler/<header>.h" or '
            '#include "core/<header>.h".',
            file=sys.stderr,
        )
        return 1

    print(f"OK: {count} test files scanned, 0 broken includes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
