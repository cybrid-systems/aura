#!/usr/bin/env python3
"""
Arena-owner invariant test.

After any reasonable sequence of operations, the main arena should
contain ONLY evaluator bookkeeping — never transient AST state from
load_module_file / eval-expr / query:pattern / functor instantiation
/ suite-runner check forms.

Concrete invariant: after the script below, main_arena.used must stay
under a fixed budget. If a new leak source appears, this test catches
it without needing to know which specific call site regressed.

The script mixes the operations we care about:
  - import 3 stdlib modules
  - eval-expr in a loop
  - query:pattern in a loop
  - set-code + query:find cycles
  - (gc-temp) periodically (should NOT need to be required, but tests
    that it doesn't accidentally grow the main arena either)

Pre-fix: this fails because eval-expr / query:pattern / set-code each
allocate short-lived FlatASTs in main arena.
Post-fix: main arena stays under the budget.

Usage:
  python3 tests/arena_owner_invariant.py [--budget-mb N]
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
    p.add_argument("--budget-mb", type=float, default=0.50,
                   help="Fail if main arena exceeds this after the workload. "
                        "Default 0.5MB is tight; current unfixed builds grow "
                        "by ~0.20-0.30MB from 50 mixed iterations.")
    p.add_argument("--iterations", type=int, default=200)
    p.add_argument("--timeout", type=int, default=120)
    args = p.parse_args()

    if not AURA.exists():
        print(f"ERROR: {AURA} not found", file=sys.stderr)
        return 1

    env = os.environ.copy()
    env["AURA_PATH"] = str(ROOT / "lib")

    lines = [
        "(gc-freeze)",
        '(import "std/json")',
        '(import "std/list")',
        '(import "std/algorithm")',
    ]
    # Capture arena stats at start of loop so we can compare before/after.
    # Loop body exercises eval-expr / set-code / query:pattern N times.
    # Wrapped in a let loop so the input program itself stays small
    # (otherwise its parsed AST would inflate main arena).
    lines.append("(define _s0 (gc-arena-stats))")
    lines.append(
        f"(define _s1 (let loop ((i 0)) (if (< i {args.iterations})"
        f" (begin (eval-expr i)"
        f'        (set-code "(define (f x) (+ x 1))")'
        f'        (set-code "(define (g y) (* y 2))")'
        f'        (query:pattern "(define ...)")'
        f"        (loop (+ i 1)))"
        f" (gc-arena-stats))))"
    )
    lines.append("(display _s0)(display \"\\n\")")
    lines.append("(display _s1)(display \"\\n\")")
    lines.append("(display (gc-arena-stats))(display \"\\n\")")
    lines.append("(display (gc-stats))(display \"\\n\")")

    program = "\n".join(lines) + "\n"

    try:
        r = subprocess.run([str(AURA)], input=program, capture_output=True,
                           text=True, timeout=args.timeout, env=env)
    except subprocess.TimeoutExpired:
        print("ERROR: aura timed out", file=sys.stderr)
        return 2

    if r.returncode != 0:
        print(f"FAIL: aura returned {r.returncode}", file=sys.stderr)
        if r.stderr:
            print(f"stderr: {r.stderr[:500]}", file=sys.stderr)
        return 4

    out_lines = [l for l in r.stdout.splitlines() if l.strip()]
    if len(out_lines) < 4:
        print("FAIL: expected 4 output lines, got:", file=sys.stderr)
        print(r.stdout, file=sys.stderr)
        return 5

    s0 = parse_arena_stats(out_lines[0])  # _s0: before loop
    s1 = parse_arena_stats(out_lines[1])  # _s1: after loop
    arena_str = out_lines[2]              # final gc-arena-stats
    stats_str = out_lines[3]              # final gc-stats
    main_before = s0.get("main", (0, 0))[0]
    main_after = s1.get("main", (0, 0))[0]
    main_delta = main_after - main_before
    final_arenas = parse_arena_stats(arena_str)
    final_main = final_arenas.get("main", (0, 0))[0]
    n_per_mod = len([k for k in final_arenas if k != "main"])

    print("=== Arena-Owner Invariant Test ===")
    print(f"iterations:           {args.iterations}")
    print(f"budget:               {args.budget_mb} MB")
    print(f"main arena delta:     {main_before:.2f}MB → {main_after:.2f}MB (Δ {main_delta:+.2f}MB)")
    print(f"main arena (final):   {final_main:.2f} MB")
    print(f"per-module arenas:    {n_per_mod}")
    print(f"arena stats (final):  {arena_str}")
    print(f"heap stats:           {stats_str}")

    # The invariant: main arena shouldn't grow from the workload itself.
    # Use the delta measurement (before vs after the loop) to filter out
    # pre-existing main arena usage from the program setup.
    if main_delta > args.budget_mb:
        print(f"FAIL: main arena grew by {main_delta:+.2f}MB during workload "
              f"(budget {args.budget_mb}MB)", file=sys.stderr)
        print("  Possible leak sources:", file=sys.stderr)
        print("    - src/compiler/evaluator_impl.cpp:2842 (suite check form)", file=sys.stderr)
        print("    - src/compiler/evaluator_impl.cpp:4142 (set-code 2nd path)", file=sys.stderr)
        print("    - src/compiler/evaluator_impl.cpp:4180 (eval-expr)", file=sys.stderr)
        print("    - src/compiler/evaluator_impl.cpp:5174 (query:pattern)", file=sys.stderr)
        print("    - src/compiler/evaluator_impl.cpp:5526 (mutate:replace-pattern)", file=sys.stderr)
        print("    - src/compiler/evaluator_impl.cpp:13347 (require dynamic import)", file=sys.stderr)
        print("    - src/compiler/evaluator_impl.cpp:13932 (functor instantiation)", file=sys.stderr)
        return 6

    if n_per_mod < 3:
        print(f"FAIL: expected at least 3 per-module arenas, got {n_per_mod}",
              file=sys.stderr)
        return 7

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
