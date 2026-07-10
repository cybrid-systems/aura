#!/usr/bin/env python3
"""Issue #1148: audit Evaluator::bump_* methods for dead (never-called) helpers.

Phase 1: report declared vs called counts. Exit 0 always unless --strict
is set (then exit 1 when dead rate exceeds --max-dead-rate, default 0.90).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EVAL_IXX = ROOT / "src/compiler/evaluator.ixx"
SRC = ROOT / "src"


def bump_methods() -> list[str]:
    text = EVAL_IXX.read_text(encoding="utf-8", errors="replace")
    # void bump_foo( or void bump_foo (
    found = re.findall(r"\bvoid\s+(bump_[A-Za-z0-9_]+)\s*\(", text)
    # unique preserve order
    seen: set[str] = set()
    out: list[str] = []
    for m in found:
        if m not in seen:
            seen.add(m)
            out.append(m)
    return out


def call_count(name: str) -> int:
    # ripgrep for name( excluding evaluator.ixx
    try:
        r = subprocess.run(
            [
                "rg",
                "-n",
                rf"\b{re.escape(name)}\s*\(",
                str(SRC / "compiler"),
                "--glob",
                "!evaluator.ixx",
                "--glob",
                "!observability_metrics.h",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
    except FileNotFoundError:
        return -1
    lines = [ln for ln in r.stdout.splitlines() if ln.strip()]
    return len(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--max-dead-rate", type=float, default=0.90)
    ap.add_argument("--list-dead", action="store_true")
    args = ap.parse_args()
    methods = bump_methods()
    dead: list[str] = []
    live = 0
    for m in methods:
        n = call_count(m)
        if n == 0:
            dead.append(m)
        elif n > 0:
            live += 1
    total = len(methods)
    dead_n = len(dead)
    rate = (dead_n / total) if total else 0.0
    print(f"bump_methods_total={total}")
    print(f"bump_methods_live={live}")
    print(f"bump_methods_dead={dead_n}")
    print(f"dead_rate={rate:.4f}")
    if args.list_dead:
        for m in dead[:50]:
            print(f"DEAD {m}")
        if dead_n > 50:
            print(f"... and {dead_n - 50} more")
    if args.strict and rate > args.max_dead_rate:
        print(f"STRICT FAIL: dead_rate {rate:.4f} > {args.max_dead_rate}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
