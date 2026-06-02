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
    for i in range(args.iterations):
        lines.append(f"(eval-expr {i})")
        lines.append('(set-code "(define (f x) (+ x 1))")')
        lines.append('(set-code "(define (g y) (* y 2))")')
        lines.append('(query:pattern "(define ...)")')
        if i % 10 == 9:
            lines.append("(gc-temp)")  # shouldn't be necessary; invariant says we don't need it
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
    if len(out_lines) < 2:
        print("FAIL: expected 2 output lines, got:", file=sys.stderr)
        print(r.stdout, file=sys.stderr)
        return 5

    arena_str = out_lines[-2]
    stats_str = out_lines[-1]
    arenas = parse_arena_stats(arena_str)
    main_used = arenas.get("main", (0, 0))[0]
    n_per_mod = len([k for k in arenas if k != "main"])

    print("=== Arena-Owner Invariant Test ===")
    print(f"iterations:           {args.iterations}")
    print(f"budget:               {args.budget_mb} MB")
    print(f"main arena:           {main_used:.2f} MB")
    print(f"per-module arenas:    {n_per_mod}")
    print(f"arena stats:          {arena_str}")
    print(f"heap stats:           {stats_str}")

    if main_used > args.budget_mb:
        print(f"FAIL: main arena at {main_used:.2f}MB exceeds budget {args.budget_mb}MB",
              file=sys.stderr)
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
        # 3 imports at the top should have created 3 per-module arenas.
        print(f"FAIL: expected at least 3 per-module arenas, got {n_per_mod}",
              file=sys.stderr)
        return 7

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
