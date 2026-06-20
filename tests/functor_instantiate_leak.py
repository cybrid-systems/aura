#!/usr/bin/env python3
"""
Functor instantiation leak regression test.

Functor instantiation at src/compiler/evaluator_impl.cpp:13932 has the
EXACT same pattern as the load_module_file leak we just fixed:
    auto* cached_env = arena_->create<Env>(mod_env);
    auto mod_idx = modules_.size();
    modules_.push_back(cached_env);
i.e. an arena-allocated Env is retained in `modules_` forever, with
no per-instance reset path.

Pre-fix: instantiating the same functor N times leaks 1 Env per
instantiation into the main arena. With template-heavy code (e.g.
defining a generic Stack<T> and instantiating Stack Int / Float /
String / ...), main arena grows with the number of distinct type
arguments.

Post-fix (target): functor instances should live in a per-instance
arena that can be freed via (gc-module ...).

Usage:
  python3 tests/functor_instantiate_leak.py [--iterations N]
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
    p.add_argument("--iterations", type=int, default=50)
    p.add_argument("--timeout", type=int, default=60)
    p.add_argument("--max-main-mb", type=float, default=5.0)
    args = p.parse_args()

    if not AURA.exists():
        print(f"ERROR: {AURA} not found", file=sys.stderr)
        return 1

    env = os.environ.copy()
    env["AURA_PATH"] = str(ROOT / "lib")

    # Define a trivial functor and instantiate it N times. Each (Box T)
    # call goes through the leak at line 13932.
    setup = "(define-module (Box T) (define (get x) x))"
    instantiations = "\n".join(
        f"(Box {t})" for t in (["Int", "Float", "String", "Bool", "Char"] * (args.iterations // 5 + 1))
    )[: args.iterations * 20]

    program = (
        "(gc-freeze)\n"
        f"{setup}\n"
        "(define _n0 (gc-module-count))\n"
        f"{instantiations}\n"
        "(define _n1 (gc-module-count))\n"
        "(define _s0 (gc-arena-stats))\n"
        '(display _n0)(display " ")\n'
        '(display _n1)(display " ")\n'
        '(display (- _n1 _n0))(display "\\n")\n'
        '(display _s0)(display "\\n")\n'
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
        print("FAIL: expected 2 output lines, got:", file=sys.stderr)
        print(r.stdout, file=sys.stderr)
        return 5

    # First line: "n0 n1 delta" — module counts before/after instantiations.
    counts = lines[0].split()
    if len(counts) != 3:
        print(f"FAIL: bad counts line: {lines[0]}", file=sys.stderr)
        return 5
    n0, n1, delta = int(counts[0]), int(counts[1]), int(counts[2])

    # Second line: arena stats.
    s0 = parse_arena_stats(lines[1])
    main_after = s0.get("main", (0, 0))[0]
    n_modules = len([k for k in s0 if k != "main"])

    print("=== Functor Instantiation Leak Test ===")
    print(f"iterations:           {args.iterations}")
    print(f"modules before:       {n0}")
    print(f"modules after:        {n1}")
    print(f"new modules:          {delta}")
    print(f"main arena:           {main_after:.2f}MB")
    print(f"per-instance arenas:  {n_modules}")
    print(f"arena stats:          {lines[1]}")

    # With the 5 distinct types cycled, functor_instance_cache_ should
    # dedupe — so delta should be 5 (one per distinct type), not
    # args.iterations. Each duplicate instantiation must NOT grow modules_.
    expected_new = 5  # Int/Float/String/Bool/Char
    if delta > expected_new + 2:  # small slack for any module the define-module itself adds
        print(
            f"FAIL: instantiating the same 5 functors {args.iterations} times "
            f"added {delta} modules (expected {expected_new}). "
            f"Likely cause: functor instance cache not deduping, OR "
            f"src/compiler/evaluator_impl.cpp:13932 leaking per instantiation.",
            file=sys.stderr,
        )
        return 6
    if main_after > args.max_main_mb:
        print(
            f"FAIL: main arena at {main_after:.2f}MB exceeds {args.max_main_mb}MB",
            file=sys.stderr,
        )
        return 7
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
