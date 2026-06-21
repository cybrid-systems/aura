#!/usr/bin/env python3
"""
query:pattern / mutate:replace-pattern leak regression test.

(query:pattern "...") in evaluator_primitives_query_workspace.cpp and
mutate:replace-pattern in evaluator_primitives_mutate.cpp both allocate a fresh
StringPool + FlatAST in the main arena for every call to parse the
pattern. Like eval-expr, the FlatAST is short-lived but the main arena
is monotonic.

Pre-fix: each (query:pattern ...) grows main arena by ~O(pattern size).
Post-fix (target): pattern parsing should route through temp_arena_ or
per-call sub-arena.

Usage:
  python3 tests/query_pattern_leak.py [--iterations N] [--op pattern|replace]
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AURA = ROOT / "build" / "aura"


def parse_arena_stats(s: str) -> dict[str, tuple[float, float]]:
    out: dict[str, tuple[float, float]] = {}
    for entry in s.split(";"):
        if ":" not in entry:
            continue
        name, rest = entry.split(":", 1)
        m = re.match(r"([0-9.]+)MB/([0-9.]+)MB", rest)
        if m:
            out[name] = (float(m.group(1)), float(m.group(2)))
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--iterations", type=int, default=1000)
    p.add_argument(
        "--op",
        choices=["pattern", "replace"],
        default="pattern",
        help="Which leak source to exercise",
    )
    p.add_argument("--timeout", type=int, default=60)
    p.add_argument(
        "--max-main-mb",
        type=float,
        default=0.20,
        help="Fail if main arena exceeds this. 200 (query:pattern ...) "
        "currently grows main arena by ~0.30MB on unfixed builds.",
    )
    args = p.parse_args()

    if not AURA.exists():
        print(f"ERROR: {AURA} not found", file=sys.stderr)
        return 1

    env = os.environ.copy()
    env["AURA_PATH"] = str(ROOT / "lib")

    # Set up a small workspace so query:pattern / mutate:replace-pattern
    # have something to work against. Use a let loop so the input program
    # itself is small (otherwise the parsed AST inflates main arena).
    setup = '(set-code "(define (f x) (+ x 1)) (define (g x) (* x 2)) (define (h a b) (+ a b)) (define (i x) (* x 3))")'
    if args.op == "pattern":
        loop_body = '(query:pattern "(define (F _ _) (+ _ _))")'
    else:  # replace
        loop_body = '(mutate:replace-pattern "(define (F _ _))" "(define (F x y))" "iter")'
    program = (
        "(gc-freeze)\n"
        f"{setup}\n"
        "(define _s0 (gc-arena-stats))\n"
        f"(define _s1 (let loop ((i 0)) (if (< i {args.iterations})"
        f" (begin {loop_body} (loop (+ i 1)))"
        f" (gc-arena-stats))))"
        '(display _s0)(display "\\n")\n'
        '(display _s1)(display "\\n")\n'
    )

    try:
        r = subprocess.run(
            [str(AURA)],
            input=program,
            capture_output=True,
            text=True,
            timeout=args.timeout,
            env=env,
        )
    except subprocess.TimeoutExpired:
        print("ERROR: aura timed out", file=sys.stderr)
        return 2

    if r.returncode != 0:
        print(f"FAIL: aura returned {r.returncode}", file=sys.stderr)
        if r.stderr:
            print(f"stderr: {r.stderr[:500]}", file=sys.stderr)
        return 4

    lines = [line for line in r.stdout.splitlines() if line.strip()]
    if len(lines) < 2:
        print("FAIL: expected 2 arena-stats lines, got:", file=sys.stderr)
        print(r.stdout, file=sys.stderr)
        return 5

    s0 = parse_arena_stats(lines[-2])
    s1 = parse_arena_stats(lines[-1])

    print(f"=== {args.op} Leak Test ===")
    print(f"iterations: {args.iterations}")
    main_before = s0.get("main", (0, 0))[0]
    main_after = s1.get("main", (0, 0))[0]
    main_delta = main_after - main_before
    print(f"main arena: {main_before:.2f}MB → {main_after:.2f}MB (Δ {main_delta:+.2f}MB)")
    if main_after > args.max_main_mb:
        print(
            f"FAIL: main arena at {main_after:.2f}MB exceeds {args.max_main_mb}MB",
            file=sys.stderr,
        )
        return 6
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
