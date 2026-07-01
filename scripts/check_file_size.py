#!/usr/bin/env python3
"""check_file_size.py — Issue #382 file size policy enforcer.

Walks src/ for .ixx files and reports any file over the warning
threshold. Exit code:
  0  no file is over the blocker threshold
  1  at least one file is over the blocker threshold (CI fail)
  2  script misuse (bad arguments, src/ missing)

Two thresholds (configurable via CLI flags):
  --warning N    lines count that triggers a warning (default 800)
  --blocker N    lines count that triggers a CI failure (default 2000)

Why two thresholds? Many existing .ixx files are over the
800-line "target" but we don't want to fail CI for all of them
on day 1. The blocker (2000) is the hard limit — anything
over it is considered a bug that should be split before merging
new code. Files over the warning but under the blocker are
flagged for the next split cycle.

The policy itself is documented in scripts/file_size_policy.md.

Usage:
  ./scripts/check_file_size.py                  # default thresholds
  ./scripts/check_file_size.py --warning 1000   # custom warning
  ./scripts/check_file_size.py --blocker 1500   # custom blocker
  ./scripts/check_file_size.py --json           # machine-readable output

Exit codes:
  0  clean (no blockers)
  1  at least one blocker found
  2  script error

Issue #382 scope-limited first cut. The full AC asks for
clang-tidy integration; this script is the simpler, dependency-
free alternative that covers the same use case (CI flagging
oversized files).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class FileReport:
    path: str
    lines: int
    status: str  # "ok" | "warning" | "blocker"


def count_lines(path: Path) -> int:
    """Count newlines in a file. O(1) memory for big files."""
    count = 0
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            count += chunk.count(b"\n")
    return count


def walk_ixx_files(src_root: Path) -> list[Path]:
    """Find all .ixx files under src/. Skips hidden dirs + .git."""
    if not src_root.is_dir():
        return []
    files = []
    for dirpath, dirnames, filenames in os.walk(src_root):
        # Skip hidden / vendored dirs
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        for name in filenames:
            if name.endswith(".ixx"):
                files.append(Path(dirpath) / name)
    return sorted(files)


def classify(lines: int, warning: int, blocker: int) -> str:
    if lines >= blocker:
        return "blocker"
    if lines >= warning:
        return "warning"
    return "ok"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Enforce Issue #382 file size policy on .ixx files",
    )
    parser.add_argument(
        "--src",
        default="src",
        help="Source root directory (default: src)",
    )
    parser.add_argument(
        "--warning",
        type=int,
        default=800,
        help="Warning threshold in lines (default: 800)",
    )
    parser.add_argument(
        "--blocker",
        type=int,
        default=2000,
        help="Blocker threshold in lines (default: 2000)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Emit machine-readable JSON output instead of human-readable text",
    )
    args = parser.parse_args()

    # Sanity-check the thresholds.
    if args.warning <= 0 or args.blocker <= 0:
        print(f"error: thresholds must be positive (warning={args.warning}, blocker={args.blocker})", file=sys.stderr)
        return 2
    if args.blocker < args.warning:
        print(f"error: blocker ({args.blocker}) must be >= warning ({args.warning})", file=sys.stderr)
        return 2

    src_root = Path(args.src)
    if not src_root.is_dir():
        print(f"error: source root '{src_root}' is not a directory", file=sys.stderr)
        return 2

    files = walk_ixx_files(src_root)
    if not files:
        print(f"error: no .ixx files found under '{src_root}'", file=sys.stderr)
        return 2

    reports = [
        FileReport(
            path=str(f.relative_to(src_root.parent)) if src_root.parent else str(f),
            lines=count_lines(f),
            status="",
        )
        for f in files
    ]
    classified = [
        FileReport(path=r.path, lines=r.lines, status=classify(r.lines, args.warning, args.blocker)) for r in reports
    ]

    blockers = [r for r in classified if r.status == "blocker"]
    warnings = [r for r in classified if r.status == "warning"]
    ok = [r for r in classified if r.status == "ok"]

    if args.json:
        out = {
            "thresholds": {"warning": args.warning, "blocker": args.blocker},
            "summary": {
                "total": len(classified),
                "ok": len(ok),
                "warning": len(warnings),
                "blocker": len(blockers),
            },
            "files": [{"path": r.path, "lines": r.lines, "status": r.status} for r in classified],
        }
        print(json.dumps(out, indent=2))
    else:
        # Human-readable report.
        print(f"Issue #382 file size policy — {len(classified)} .ixx files")
        print(f"  warning threshold: {args.warning} lines")
        print(f"  blocker threshold: {args.blocker} lines")
        print()
        if blockers:
            print(f"BLOCKERS ({len(blockers)}) — must be split before merging new code:")
            for r in sorted(blockers, key=lambda x: -x.lines):
                print(f"  {r.lines:>5d}  {r.path}")
            print()
        if warnings:
            print(f"WARNINGS ({len(warnings)}) — schedule for next split cycle:")
            for r in sorted(warnings, key=lambda x: -x.lines):
                print(f"  {r.lines:>5d}  {r.path}")
            print()
        print(f"OK ({len(ok)} files under warning threshold)")

    return 1 if blockers else 0


if __name__ == "__main__":
    sys.exit(main())
