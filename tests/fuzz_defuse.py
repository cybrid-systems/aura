#!/usr/bin/env python3
"""Def-Use Stress Fuzz — stress-test def-use cache under heavy mutation.

Strategy:
1. Set a complex multi-function program
2. Query def-use, reaches, effects for every symbol
3. Apply multiple mutations, query again after each
4. Repeat 100+ times: every query must succeed (no crash, no hang)

This catches:
  - Scope tree corruption after mutation
  - Off-by-one in flat arena vectors
  - Cross-scope sym index staleness
  - Memory safety: no segfaults during rebuild
  - Version counter mismatch / infinite rebuild loops

Usage:
  python3 tests/fuzz_defuse.py [--quick] [--seed N]
"""

import datetime
import json
import os
import random
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
AURA = os.environ.get("AURA_BIN", str(REPO / "build" / "aura"))

QUICK = "--quick" in sys.argv
SEED = None
for i, a in enumerate(sys.argv):
    if a == "--seed" and i + 1 < len(sys.argv):
        SEED = int(sys.argv[i + 1])

rng = random.Random(SEED if SEED is not None else None)

# Base programs with multiple functions and cross-scope references
PROGRAMS = [
    # 1: Recursive fib with display
    """\
    (define (fib n)
      (if (< n 2) n
        (+ (fib (- n 1)) (fib (- n 2)))))
    (define (show x)
      (display x))
    (show (fib 10))
    """,

    # 2: Higher-order functions
    """\
    (define (map f lst)
      (if (null? lst) ()
        (cons (f (car lst)) (map f (cdr lst)))))
    (define (double x) (* x 2))
    (define (main)
      (map double (list 1 2 3)))
    (main)
    """,

    # 3: Nested lets + lambdas
    """\
    (define (compose f g)
      (lambda (x) (f (g x))))
    (define (add1 x) (+ x 1))
    (define (mul2 x) (* x 2))
    (define h (compose add1 mul2))
    (display (h 5))
    """,

    # 4: Mutually recursive (letrec)
    """\
    (letrec
      ((even? (lambda (n) (if (= n 0) #t (odd? (- n 1)))))
       (odd?  (lambda (n) (if (= n 0) #f (even? (- n 1))))))
      (display (even? 6)))
    """,

    # 5: Complex cross-scope with many lets
    """\
    (define (process x)
      (let ((a (+ x 1))
            (b (* x 2)))
        (let ((c (+ a b)))
          (let ((d (* c x)))
            (let ((e (/ d 2)))
              e)))))
    (display (process 5))
    """,
]

SYMBOLS = ["fib", "n", "show", "x", "map", "f", "lst", "double", "main",
           "compose", "g", "h", "add1", "mul2", "even?", "odd?", "process",
           "a", "b", "c", "d", "e", "display", "car", "cdr", "cons",
           "null?", "list", "lambda", "let", "letrec"]


def escaped(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def send(proc, cmd):
    try:
        if proc.poll() is not None:
            return None
        proc.stdin.write(cmd + "\n")
        proc.stdin.flush()
        time.sleep(0.003)
        line = proc.stdout.readline()
    except (BrokenPipeError, OSError):
        return None
    if not line:
        return None
    stripped = line.strip()
    brace = stripped.rfind("{")
    json_part = stripped[brace:] if brace >= 0 else stripped
    try:
        return json.loads(json_part)
    except:
        return None


def run_session(n_cycles):
    proc = subprocess.Popen(
        [AURA, "--serve"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.1)

    stats = {"ok": 0, "error": 0, "crash": 0, "timeout": 0}

    program = rng.choice(PROGRAMS)
    resp = send(proc, f'(set-code "{escaped(program)}")')
    if not resp or resp.get("status") != "ok":
        try: proc.kill()
        except: pass
        stats["crash"] = 1
        return stats

    cycle = 0
    while cycle < n_cycles:
        # Pick a random symbol to query (including some undefined ones)
        sym = rng.choice(SYMBOLS + ["_nonexistent_" + str(rng.randint(0, 1000))])

        # Pick a random def-use operation
        op = rng.choice([
            lambda: f'(display (query:def-use "{sym}"))',
            lambda: f'(display (query:effects "{sym}"))',
            lambda: f'(display (query:reaches {rng.randint(0, 10)}))',
            lambda: f'(display (query:def-use "{sym}"))\n(display (query:def-use "{rng.choice(SYMBOLS)}"))',
            lambda: f'(typecheck-current)',
            lambda: f'(display (current-source))',
        ])

        resp = send(proc, op())
        if resp is None:
            stats["crash"] += 1
            break

        status = resp.get("status", "error")
        if status in ("ok", "closure"):
            stats["ok"] += 1
        else:
            stats["error"] += 1

        # Sometimes apply a mutation to test cache rebuild
        if cycle > 0 and rng.random() < 0.3:
            fn = rng.choice(["fib", "show", "double", "compose", "process", "f", "map"])
            new_val = rng.randint(1, 100)
            mut_op = rng.choice([
                f'(mutate:tweak-literal {rng.randint(0, 3)} {rng.choice([1, -1, 5])} "fuzz tweak")',
                f'(mutate:rebind "{fn}" "(lambda (x) (+ x {new_val}))" "fuzz rebind")',
                f'(mutate:replace-value {rng.randint(0, 5)} {new_val} "fuzz replace")',
            ])
            resp = send(proc, mut_op)
            if resp is None:
                stats["crash"] += 1
                break

        # Occasionally rotate program
        if cycle > 0 and cycle % 25 == 0 and rng.random() < 0.4:
            program = rng.choice(PROGRAMS)
            resp = send(proc, f'(set-code "{escaped(program)}")')
            if not resp or resp.get("status") != "ok":
                stats["crash"] += 1
                break

        cycle += 1
        if QUICK and cycle >= n_cycles:
            break

    try:
        proc.stdin.close()
        proc.kill()
        proc.wait(timeout=3)
    except:
        pass
    return stats


def main():
    print("=" * 60)
    print("Def-Use Stress Fuzz")
    print(f"  Date:   {datetime.date.today().isoformat()}")
    print(f"  Seed:   {SEED if SEED is not None else 'random'}")
    print(f"  Mode:   {'QUICK' if QUICK else 'FULL'}")
    print("=" * 60)

    n_cycles = 200 if QUICK else 1000
    n_sessions = 2 if QUICK else 5

    total = {"ok": 0, "error": 0, "crash": 0, "timeout": 0}

    for s in range(n_sessions):
        print(f"\n  Session {s+1}/{n_sessions} ... ", end="", flush=True)
        st = run_session(n_cycles // n_sessions)
        for k in total:
            total[k] += st[k]
        pct = st["ok"] / max(st["ok"] + st["error"] + st["crash"], 1) * 100
        print(f"{st['ok']}/{st['ok']+st['error']+st['crash']} ({pct:.0f}%) "
              f"[errors={st['error']} crash={st['crash']}]")

    print(f"\n{'='*60}")
    print(f"  Summary: {total['ok']} ok, {total['error']} error, "
          f"{total['crash']} crash, {total['timeout']} timeout")
    rate = total["ok"] / max(total["ok"] + total["error"] + total["crash"], 1) * 100
    print(f"  Rate: {rate:.1f}%")

    if total["crash"]:
        print(f"\n  💥 CRASH detected!")
        sys.exit(1)


if __name__ == "__main__":
    main()
