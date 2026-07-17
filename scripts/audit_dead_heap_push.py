#!/usr/bin/env python3
"""Issue #1488 / #1072: audit dead string_heap_ push patterns.

Flags sites where an index is taken from string_heap_.size(), a push_back
follows, and the index is never used (incomplete-refactor leftover that
pollutes the heap on every call).

Usage:
  python3 scripts/audit_dead_heap_push.py          # report, exit 0
  python3 scripts/audit_dead_heap_push.py --strict # exit 1 if any candidate

Exit codes:
  0 — clean (or report-only with findings)
  1 — --strict and at least one candidate
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"

SIZE_RE = re.compile(
    r"(?:auto|const\s+auto|std::size_t|size_t)\s+(\w+)\s*=\s*"
    r"(?:ev\.)?string_heap_\.size\(\)\s*;"
)
PUSH_RE = re.compile(r"string_heap_\.(?:push_back|emplace_back)\s*\(")


def scan_file(path: Path) -> list[tuple[int, str, str]]:
    """Return (line_no, var_name, snippet) for dead candidates."""
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    hits: list[tuple[int, str, str]] = []
    for i, line in enumerate(lines):
        m = SIZE_RE.search(line)
        if not m:
            continue
        name = m.group(1)
        push_at: int | None = None
        for j in range(i + 1, min(len(lines), i + 6)):
            if PUSH_RE.search(lines[j]):
                push_at = j
                break
        if push_at is None:
            continue
        used = False
        for j in range(push_at + 1, min(len(lines), i + 80)):
            wl = re.sub(r"//.*", "", lines[j])
            if re.search(rf"\b{re.escape(name)}\b", wl):
                used = True
                break
        if not used:
            snippet = lines[i].strip()
            hits.append((i + 1, name, snippet))
    return hits


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 when any dead candidate is found",
    )
    ap.add_argument(
        "--path",
        type=Path,
        default=SRC,
        help="directory to scan (default: src/)",
    )
    args = ap.parse_args()

    files = sorted(args.path.rglob("*.cpp")) + sorted(args.path.rglob("*.ixx"))
    all_hits: list[tuple[Path, int, str, str]] = []
    for f in files:
        for ln, name, snip in scan_file(f):
            all_hits.append((f.relative_to(ROOT), ln, name, snip))

    if not all_hits:
        print("audit_dead_heap_push: clean (0 candidates)")
        return 0

    print(f"audit_dead_heap_push: {len(all_hits)} candidate(s)")
    for path, ln, name, snip in all_hits:
        print(f"  {path}:{ln}: unused '{name}' after string_heap_ push")
        print(f"    {snip}")

    if args.strict:
        print("audit_dead_heap_push: FAIL (--strict)", file=sys.stderr)
        return 1
    print("audit_dead_heap_push: report-only (use --strict to gate)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
