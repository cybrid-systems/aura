#!/usr/bin/env python3
"""Rank query:*-stats (and compile:*-stats) usage across the tree.

Issue #1434 / P1b — identify top callers for facade migration.

Usage:
  python3 scripts/find_top_stats.py
  python3 scripts/find_top_stats.py --top 20 --json
  python3 scripts/find_top_stats.py --direct-only   # exclude engine:metrics lines
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCAN_ROOTS = ("src", "tests", "demos", "lib")
EXTS = {".cpp", ".ixx", ".h", ".hpp", ".aura", ".c", ".py"}

# Names like query:foo-stats, compile:bar-stats-hash
NAME_RE = re.compile(r"\b((?:query|compile):[a-zA-Z0-9_.:-]*-stats(?:-hash)?)\b")


def iter_files():
    for root_name in SCAN_ROOTS:
        root = ROOT / root_name
        if not root.is_dir():
            continue
        for p in root.rglob("*"):
            if p.suffix not in EXTS:
                continue
            if "build" in p.parts or "__pycache__" in p.parts:
                continue
            yield p


def scan(*, direct_only: bool) -> tuple[Counter[str], dict[str, set[str]]]:
    counts: Counter[str] = Counter()
    files: dict[str, set[str]] = {}
    for path in iter_files():
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        rel = str(path.relative_to(ROOT))
        for line in text.splitlines():
            # Skip pure registration add("…") lines as non-callers
            if re.search(r'\badd\s*\(\s*"', line) or re.search(r"\badd\s*\(\s*'", line):
                continue
            if direct_only and "engine:metrics" in line:
                # Still count if there's a bare (query:…) on same line? rare — skip whole line
                continue
            for m in NAME_RE.finditer(line):
                name = m.group(1)
                counts[name] += 1
                files.setdefault(name, set()).add(rel)
    return counts, files


# Frozen top-20 used by #1434 migration (recomputed; pinned for stability).
# Re-run without --pin to refresh ranking.
PINNED_TOP20 = [
    "query:envframe-dualpath-stats",
    "query:self-evolution-closedloop-stats",
    "query:macro-reflect-validation-stats",
    "query:macro-jit-hygiene-stats",
    "query:pattern-index-stats",
    "query:stable-ref-layer-stats",
    "query:arena-auto-compact-defrag-fiber-stats",
    "query:pattern-stats",
    "query:typed-mutation-stats",
    "query:fiber-boundary-violation-stats",
    "query:value-dispatch-stats",
    "query:incremental-relower-stats",
    "query:edsl-reflection-stats",
    "query:panic-checkpoint-lifecycle-stats",
    "query:jit-interpreter-parity-stats",
    "query:closure-env-epoch-safety-stats",
    "query:arena-integration-stats",
    "query:pattern-hygiene-stats",
    "query:macro-provenance-stats",
    "query:envframe-dualpath-policy-stats",
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--json", action="store_true")
    ap.add_argument(
        "--direct-only",
        action="store_true",
        help="Ignore lines that already use engine:metrics",
    )
    ap.add_argument(
        "--print-pinned",
        action="store_true",
        help="Print the frozen #1434 top-20 list",
    )
    args = ap.parse_args()

    if args.print_pinned:
        for n in PINNED_TOP20:
            print(n)
        return 0

    counts, files = scan(direct_only=args.direct_only)
    ranked = counts.most_common(args.top)

    if args.json:
        payload = [{"name": n, "count": c, "files": sorted(files.get(n, []))} for n, c in ranked]
        json.dump({"top": payload, "unique": len(counts)}, sys.stdout, indent=2)
        print()
        return 0

    mode = "direct-only" if args.direct_only else "all refs"
    print(f"Top {args.top} stats primitives by usage ({mode}):")
    print(f"{'count':>6}  {'files':>5}  name")
    for name, c in ranked:
        print(f"{c:6d}  {len(files.get(name, [])):5d}  {name}")
    print(f"\nunique names: {len(counts)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
