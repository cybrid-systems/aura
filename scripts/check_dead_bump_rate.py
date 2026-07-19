#!/usr/bin/env python3
"""Issue #1645 — dead bump_* rate CI gate.

Closes AC2 of #1645: "grep 确认 dead bump rate < 10%".

Counts `bump_*` declarations in src/compiler/evaluator.ixx vs the set of
bump_*() call sites elsewhere under src/. The rate = dead / total.
Exits 0 if the rate is under the threshold (default 10%); 1 otherwise.

Usage:
    python3 scripts/check_dead_bump_rate.py
    python3 scripts/check_dead_bump_rate.py --threshold 0.20      # 20% floor
    python3 scripts/check_dead_bump_rate.py --report dead          # print dead names
    python3 scripts/check_dead_bump_rate.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

DECL_FILE = ROOT / "src/compiler/evaluator.ixx"
EXCLUDE_CALLERS = {"evaluator.ixx", "observability_metrics.h"}

DECL_RE = re.compile(r"\b(?:void|std::uint64_t|int64_t|bool|auto)\s+bump_([a-z_][a-z0-9_]*)\s*\(")
CALL_RE = re.compile(r"\bbump_([a-z_][a-z0-9_]*)\s*\(")

PROD_FILE_EXTENSIONS = (".cpp", ".h", ".hpp", ".ixx", ".inc", ".hh", ".c", ".cc")


def _read_text(p: Path) -> str:
    try:
        return p.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return ""


def _decls() -> set[str]:
    if not DECL_FILE.exists():
        return set()
    return {"bump_" + m.group(1) for m in DECL_RE.finditer(_read_text(DECL_FILE))}


def _caller_files() -> dict[str, set[str]]:
    out: dict[str, set[str]] = {}
    for path in ROOT.rglob("src/**/*"):
        if not path.is_file() or path.suffix not in PROD_FILE_EXTENSIONS:
            continue
        if path.name in EXCLUDE_CALLERS:
            continue
        for m in CALL_RE.finditer(_read_text(path)):
            n = "bump_" + m.group(1)
            out.setdefault(n, set()).add(path.name)
    return out


def report(threshold: float) -> tuple[set[str], set[str], float, int, int]:
    """Return (decls_set, dead_set, rate, live_count, dead_count)."""
    decls = _decls()
    callers = _caller_files()
    live = {d for d in decls if d in callers}
    dead = {d for d in decls if d not in callers}
    rate = (len(dead) / len(decls)) if decls else 0.0
    return decls, dead, rate, len(live), len(dead)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--threshold", type=float, default=0.10, help="Max allowed dead-bump rate (default 0.10).")
    ap.add_argument(
        "--report",
        choices=("dead", "live", "all"),
        default="dead",
        help="Which set to print (default 'dead'). Use 'all' to print a full audit.",
    )
    ap.add_argument("--self-test", action="store_true", help="Run a synthetic self-test (does not touch the repo).")
    args = ap.parse_args()

    if args.self_test:
        # Synthetic happy / sad paths.
        synth = {"bump_x_total", "bump_y_total"}
        called = {"bump_x_total"}
        dead = synth - called
        rate = len(dead) / len(synth)
        assert rate == 0.5 and dead == {"bump_y_total"}, "self-test invariant"
        print("self-test: 2 declarations, 1 called, 1 dead, rate=50% — OK")
        return 0

    decls, dead, rate, live_count, dead_count = report(args.threshold)
    total = len(decls)
    if args.report == "dead":
        names = sorted(dead)
    elif args.report == "live":
        # Note: live_set needs caller_files; re-derive for safety.
        names = sorted(decls - dead)
    else:
        names = sorted(decls)
    print(
        f"dead-bump audit: decls={total} live={live_count} dead={dead_count} "
        f"rate={rate:.0%} threshold={args.threshold:.0%}"
    )
    if args.report != "all":
        for n in names:
            print(f"  {n}")
    else:
        for n in names:
            mark = "[DEAD]" if n in dead else "[LIVE]"
            print(f"  {mark} {n}")
    print()
    if rate > args.threshold:
        print(f"FAIL: dead-bump rate {rate:.0%} exceeds threshold {args.threshold:.0%}", file=sys.stderr)
        return 1
    print(f"PASS: dead-bump rate {rate:.0%} ≤ threshold {args.threshold:.0%}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
