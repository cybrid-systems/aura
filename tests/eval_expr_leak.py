#!/usr/bin/env python3
"""
eval-expr leak regression test.

(eval-expr value) in evaluator_primitives_eval.cpp allocates
a fresh StringPool + FlatAST in the main arena for every call. The
FlatAST is short-lived (only used for the duration of eval_flat inside
the call), but the main arena is monotonic so the memory is never
reclaimed.

Pre-fix: repeated (eval-expr ...) grows the main arena linearly.
Post-fix (target): each (eval-expr ...) should be O(1) amortized in
arena terms — ideally by routing the parse through the temp_arena_,
which is bulk-reset on (gc-temp).

Usage:
  python3 tests/eval_expr_leak.py [--iterations N]
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
    p.add_argument("--timeout", type=int, default=60)
    p.add_argument(
        "--max-main-mb",
        type=float,
        default=0.20,
        help="Fail if main arena exceeds this after the loop. "
        "200 (eval-expr int) currently grows main arena by ~0.30MB "
        "on unfixed builds; the post-fix target is sub-budget. "
        "Bump iterations or the input expression's size to stress harder.",
    )
    args = p.parse_args()

    if not AURA.exists():
        print(
            f"ERROR: {AURA} not found; run 'python3 build.py build' first",
            file=sys.stderr,
        )
        return 1

    env = os.environ.copy()
    env["AURA_PATH"] = str(ROOT / "lib")

    # Call (eval-expr ...) N times via a let loop so the INPUT program
    # itself is small (otherwise the program's parsed AST inflates the
    # main arena, drowning the signal). Each eval-expr allocates a
    # StringPool + FlatAST in temp_arena_ post-fix (line 4180) — with
    # the migration, main arena should stay at 0.0MB.
    # The let loop's value is the final (gc-arena-stats) string, so
    # we can capture it without scope-escape issues.
    program = (
        "(gc-freeze)"
        "(define _s0 (gc-arena-stats))"
        f"(define _s1 (let loop ((i 0)) (if (< i {args.iterations})"
        f" (begin (eval-expr i) (loop (+ i 1)))"
        f" (gc-arena-stats))))"
        '(display _s0)(display "\\n")'
        '(display _s1)(display "\\n")'
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

    print("=== eval-expr Leak Test ===")
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
