#!/usr/bin/env python3
"""Issue #1452 / #1453 foundation: primitive source changes must touch tests.

When production primitive registration / evaluator core changes, require a
paired change under tests/ (C++ issue tests or EDSL suite). Prevents
“land prim without AC” regressions.

Usage:
  python3 scripts/check_test_binding.py
  python3 scripts/check_test_binding.py --base origin/main
  python3 scripts/check_test_binding.py --files a.cpp b.cpp   # explicit list

Exit 0 = OK, 1 = binding violation.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

PROD_GLOBS = (
    re.compile(r"^src/compiler/evaluator_primitives.*\.cpp$"),
    re.compile(r"^src/compiler/evaluator\.ixx$"),
    re.compile(r"^src/compiler/observability_prims_decl\.inc$"),
    re.compile(r"^src/compiler/primitives_detail\.h$"),
    re.compile(r"^src/compiler/primitives_meta\.h$"),
)

TEST_PREFIXES = (
    "tests/",
    "lib/std/edsl-test-harness.aura",
    "lib/std/test.aura",
)

# Docs-only / generated paths never force a test binding by themselves.
DOC_ONLY = re.compile(r"^(docs/|scripts/check_|\.github/)")


def is_prod(path: str) -> bool:
    path = path.replace("\\", "/")
    return any(g.match(path) for g in PROD_GLOBS)


def is_test(path: str) -> bool:
    path = path.replace("\\", "/")
    return any(path.startswith(p) or path == p for p in TEST_PREFIXES)


def git_diff_names(base: str) -> list[str]:
    # Prefer merge-base range against base; fall back to working tree vs HEAD.
    cmds = [
        ["git", "diff", "--name-only", f"{base}...HEAD"],
        ["git", "diff", "--name-only", base],
        ["git", "diff", "--name-only", "HEAD"],
        ["git", "diff", "--name-only", "--cached"],
    ]
    names: set[str] = set()
    for cmd in cmds:
        try:
            r = subprocess.run(
                cmd,
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
        except FileNotFoundError:
            return []
        if r.returncode != 0:
            continue
        for line in r.stdout.splitlines():
            line = line.strip()
            if line:
                names.add(line)
        if names:
            break
    return sorted(names)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--base",
        default="origin/main",
        help="git base ref for diff (default: origin/main)",
    )
    ap.add_argument(
        "--files",
        nargs="*",
        help="explicit file list (skips git)",
    )
    ap.add_argument(
        "--soft",
        action="store_true",
        help="print warning but exit 0 on violation",
    )
    args = ap.parse_args()

    files = list(args.files) if args.files else git_diff_names(args.base)
    if not files:
        print("OK: check_test_binding (no changed files detected)")
        return 0

    prod = [f for f in files if is_prod(f)]
    tests = [f for f in files if is_test(f)]

    if not prod:
        print(f"OK: check_test_binding ({len(files)} files, no production prim sources)")
        return 0

    if tests:
        print(f"OK: check_test_binding — {len(prod)} production file(s) paired with {len(tests)} test file(s)")
        for f in prod[:8]:
            print(f"  prod: {f}")
        for f in tests[:8]:
            print(f"  test: {f}")
        return 0

    msg = (
        "FAIL: production primitive sources changed without tests/ updates "
        "(Issue #1452/#1453 test binding).\n"
        "Production files:\n"
        + "".join(f"  + {f}\n" for f in prod)
        + "Add or update tests under tests/ (C++ issue test or EDSL suite), "
        "or document an exception in the PR.\n"
        "Policy: docs/design/testing-framework-v1.md"
    )
    print(msg, file=sys.stderr)
    return 0 if args.soft else 1


if __name__ == "__main__":
    sys.exit(main())
