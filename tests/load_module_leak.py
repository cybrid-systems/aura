#!/usr/bin/env python3
"""
Module-load leak regression test.

Exercises load_module_file() via repeated (import "std/...") calls and
reports the (gc-arena-stats) telemetry before / after. With the
per-module arena fix, repeated imports should hit the module cache
and add zero new allocation to any per-module arena.

Before the fix:
  - Each import allocated StringPool + FlatAST + Env in the main arena
    and they leaked forever (main arena is monotonic, no reset).
  - gc-arena-stats would show main arena growing linearly.

After the fix:
  - Each unique module's state lives in its own arena
    (ArenaGroup::module_arena). Module cache hit → no new allocation.
  - Repeated imports leave the per-module arenas at the same usage.
  - Optional (gc-module) call resets the per-module arena to 0 used.

Usage:
  python3 tests/load_module_leak.py [--iterations N] [--gc-module]
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AURA = ROOT / "build" / "aura"


def build_program(iters: int, call_gc_module: bool, modules: list[str],
                  with_synthetic_bindings: bool) -> str:
    """Build an aura program that imports `modules` `iters` times, optionally
    calling gc-module to release per-module arenas. Optionally binds a
    function in each module to force closure retention."""
    lines = ["(gc-freeze)"]
    if with_synthetic_bindings:
        for m in modules:
            # (import ...) returns the module; bind a closure that closes
            # over a module-level function. This forces the per-module
            # arena's FlatAST/StringPool to stay live (Closure::owner_arena
            # is the per-module arena).
            lines.append(f'(import "{m}")')
            lines.append(f'(define _keep-{m.replace("/", "-")} (lambda () (import "{m}")))')
    for i in range(iters):
        for m in modules:
            lines.append(f'(import "{m}")')
        if call_gc_module and i % 4 == 3:
            for m in modules:
                lines.append(f'(gc-module "lib/{m}.aura")')
    lines.append('(display (gc-arena-stats))(display "\\n")')
    lines.append('(display (gc-stats))(display "\\n")')
    return "\n".join(lines)


def parse_arena_stats(s: str) -> dict[str, tuple[float, float]]:
    """Parse 'main:0.1MB/8.0MB;algorithm.aura:0.0MB/8.0MB;...' → dict."""
    out: dict[str, tuple[float, float]] = {}
    for entry in s.split(";"):
        if ":" not in entry:
            continue
        name, rest = entry.split(":", 1)
        m = re.match(r"([0-9.]+)MB/([0-9.]+)MB", rest)
        if m:
            out[name] = (float(m.group(1)), float(m.group(2)))
    return out


def run(args) -> int:
    if not AURA.exists():
        print(f"ERROR: {AURA} not found; run 'python3 build.py build' first",
              file=sys.stderr)
        return 1

    env = os.environ.copy()
    env["AURA_PATH"] = str(ROOT / "lib")

    program = build_program(args.iterations, args.gc_module, args.modules,
                            args.with_synthetic_bindings)
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

    lines = [l for l in r.stdout.splitlines() if l.strip()]
    if len(lines) < 1:
        print("FAIL: no output from aura", file=sys.stderr)
        return 5

    arena_str = lines[-2] if len(lines) >= 2 else lines[-1]
    stats_str = lines[-1]
    arenas = parse_arena_stats(arena_str)

    print("=== Module-Load Leak Test ===")
    print(f"iterations:    {args.iterations}")
    print(f"modules:       {args.modules}")
    print(f"gc-module:     {args.gc_module}")
    print(f"synthetic binds: {args.with_synthetic_bindings}")
    print(f"arena stats:   {arena_str}")
    print(f"heap stats:    {stats_str}")

    # Per-module arenas should exist for each imported module.
    expected = set()
    for m in args.modules:
        base = m.split("/")[-1] + ".aura"
        expected.add(base)
    got = set(k for k in arenas if k != "main")
    missing = expected - got
    if missing:
        print(f"FAIL: expected per-module arenas for {missing}, got {got}",
              file=sys.stderr)
        return 6
    print(f"per-module arenas: {sorted(got)}")

    # Main arena should NOT be eating module state. After the fix, the
    # main arena should be small (just evaluator bookkeeping + main
    # closure primitives), while each module lives in its own arena.
    main_used_mb = arenas.get("main", (0, 0))[0]
    if main_used_mb > args.max_main_mb:
        print(f"FAIL: main arena at {main_used_mb}MB (limit {args.max_main_mb}MB) — "
              f"module state likely leaked into the main arena",
              file=sys.stderr)
        return 7

    if args.gc_module:
        # After gc-module, every per-module arena should report 0.0MB used.
        for name, (used, _cap) in arenas.items():
            if name == "main":
                continue
            if used > 0.01:
                print(f"FAIL: per-module arena {name} still at {used}MB after gc-module",
                      file=sys.stderr)
                return 8
        print("per-module arenas: all 0.0MB after gc-module ✓")

    print("PASS")
    return 0


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--iterations", type=int, default=20)
    p.add_argument("--gc-module", action="store_true",
                   help="Call gc-module periodically to release per-module arenas")
    p.add_argument("--with-synthetic-bindings", action="store_true",
                   help="Bind a function in each module to force closure retention")
    p.add_argument("--modules", nargs="+",
                   default=["std/json", "std/list", "std/algorithm"])
    p.add_argument("--timeout", type=int, default=60)
    p.add_argument("--max-main-mb", type=float, default=5.0,
                   help="Fail if main arena usage exceeds this (MB). Pre-fix the "
                        "main arena would grow with every import; post-fix module "
                        "state lives in per-module arenas, so main should stay small.")
    args = p.parse_args()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
