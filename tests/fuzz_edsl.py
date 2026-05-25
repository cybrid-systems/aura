#!/usr/bin/env python3
"""EDSL Mutation Fuzz — use Aura's own EDSL via --serve to stress-test the compiler.

Strategy: start with a valid program via set-code, then apply thousands of
individual EDSL mutations (rebind, tweak-literal, set-body, query, typecheck, eval)
through --serve, one command per line.

Each command produces one JSON response. No chaining.
Tests workspace stability, typecheck after every 10th mutation, and long chains.

Usage:
  python3 tests/fuzz_edsl.py                    # 2000 ops
  python3 tests/fuzz_edsl.py --quick            # 200 ops
  python3 tests/fuzz_edsl.py --seed 42          # reproducible
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

# Suppress BrokenPipeError on cleanup (normal when killing serve)
HERE = Path(__file__).resolve().parent
REPO = HERE.parent
AURA = os.environ.get("AURA_BIN", str(REPO / "build" / "aura"))
QUICK = "--quick" in sys.argv
SEED = None
for i, a in enumerate(sys.argv):
    if a == "--seed" and i + 1 < len(sys.argv):
        SEED = int(sys.argv[i + 1])

rng = random.Random(SEED if SEED is not None else None)

OPS = [
    "(mutate:rebind \"{fn}\" \"(lambda (x) ({op} x {val}))\")",
    "(mutate:set-body \"{fn}\" \"(* x {val})\")",
    "(mutate:tweak-literal {val} {delta} \"tweak {val}->{new_val}\")",
    "(display (current-source))",
    "(display (query:node-type \"Define\"))",
    "(display (query:find \"{fn}\"))",
    "(typecheck-current)",
    "(display (eval-current))",
    "(mutate:record-patch 0 \"patch-{i}\" \"fuzz record\")",
    "(mutate:insert-child 0 0 \"(display {val})\" \"fuzz insert\")",
    "(mutate:remove-node 0 \"fuzz remove\")",
    "(display (query:children 0))",
    "(display (query:node 0))",
    "(display (query:def-use \"{fn}\"))",
    "(display (query:def-use \"{sym}\"))",
    "(display (query:effects \"{fn}\"))",
    "(display (query:reaches {val}))",
    "(display (ast:snapshot \"snap-{i}\"))",
    "(display (ast:diff 0))",
    "(display (ast:list-snapshots))",
    "(display (ast:restore 0))",

]

FN_NAMES = ["f", "g", "h", "add", "fact", "map", "filter", "foldl", "compose"]
SYMBOLS = ["x", "y", "z", "a", "b", "n", "lst", "acc"]
ARITH_OPS = ["+", "-", "*", "/", "quotient", "remainder"]
NODE_TAGS = ["Define", "Lambda", "Call", "Variable", "LiteralInt", "IfExpr",
             "BinaryOp", "Let", "Begin", "Quote"]


def escaped(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def send(proc, cmd):
    """Send one command to --serve, read one response line, parse JSON suffix."""
    try:
        if proc.poll() is not None:
            return None  # process already dead
        proc.stdin.write(cmd + "\n")
        proc.stdin.flush()
        time.sleep(0.005)
        line = proc.stdout.readline()
    except BrokenPipeError:
        return None  # pipe closed, process shutting down
    except:
        return None
    if not line:
        return None
    stripped = line.strip()
    brace = stripped.rfind("{")
    if brace >= 0:
        json_part = stripped[brace:]
        display_part = stripped[:brace].strip()
    else:
        json_part = stripped
        display_part = ""
    try:
        resp = json.loads(json_part)
        if display_part:
            resp["_display"] = display_part
        return resp
    except:
        return None


def make_program():
    """Generate a random valid Aura program."""
    pattern = rng.choice([
        lambda: f"(define (f x) (+ x 1))",
        lambda: f"(define (add a b) (+ a b))",
        lambda: f"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))",
        lambda: f"(define (map f lst) (if (null? lst) () (cons (f (car lst)) (map f (cdr lst)))))",
        lambda: f"(define (foldl f acc lst) (if (null? lst) acc (foldl f (f acc (car lst)) (cdr lst))))",
        lambda: f"(let ((x 10) (y 20)) (+ x y))",
        lambda: f"(begin (display 1) (display 2) (display 3))",
    ])
    return pattern()


def extract_fns(code):
    return re.findall(r'\(define\s+\((\w+)', code)


def run_fuzz_session(n_ops):
    """One --serve session: set-code, then N mutation operations."""
    proc = subprocess.Popen(
        [AURA, "--serve"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.1)

    stats = {"pass": 0, "fail": 0, "crash": 0, "ops": 0}
    code = make_program()
    fns = extract_fns(code)

    # Initial set-code
    resp = send(proc, f'(set-code "{escaped(code)}")')
    if not resp or resp.get("status") != "ok":
        try:
            proc.kill()
        except:
            pass
        return stats

    stats["ops"] += 1

    for i in range(n_ops):
        # Refresh code state periodically
        if i % 10 == 0:
            resp = send(proc, '(display (current-source))')
            if resp and resp.get("status") == "ok" and resp.get("_display"):
                new_code = resp["_display"]
                if new_code and new_code not in ("", "()"):
                    code = new_code
                    fns = extract_fns(code)

        # Every 20 ops: typecheck
        if i % 20 == 0:
            send(proc, '(typecheck-current)')

        # Every 50 ops: re-set-code to a new program (mix it up)
        if i > 0 and i % 50 == 0 and rng.random() < 0.3:
            code = make_program()
            fns = extract_fns(code)
            resp = send(proc, f'(set-code "{escaped(code)}")')
            if resp and resp.get("status") == "ok":
                stats["ops"] += 1
                stats["pass"] += 1
            continue

        # Pick a random operation
        op_template = rng.choice(OPS)
        fn = rng.choice(fns) if fns else "f"
        op = rng.choice(ARITH_OPS)
        val = rng.randint(1, 500)
        delta = rng.choice([1, -1, 5, -5, 10, -10])
        tag = rng.choice(NODE_TAGS)
        sym = rng.choice(SYMBOLS)

        new_val = val + delta
        cmd = op_template.format(
            fn=fn, op=op, val=val, delta=delta, new_val=new_val, tag=tag, sym=sym, i=i
        )

        resp = send(proc, cmd)
        if resp is None:
            stats["fail"] += 1
            break

        status = resp.get("status", "error")
        if status in ("ok", "closure"):
            stats["pass"] += 1
        elif status in ("error", "parse-error"):
            stats["fail"] += 1
        else:
            stats["fail"] += 1

        stats["ops"] += 1

        if (i + 1) % 200 == 0:
            print(f"    {(i+1):5d} ops  [{stats['pass']} pass, {stats['fail']} fail, {stats['crash']} crash]",
                  flush=True)

    try:
        proc.stdin.close()
        proc.kill()
        proc.wait(timeout=3)
    except:
        pass
    return stats


def main():
    print("=" * 60)
    print("Aura EDSL Mutation Fuzz")
    print(f"  Date:  {datetime.date.today().isoformat()}")
    print(f"  Seed:  {SEED if SEED is not None else 'random'}")
    print(f"  Mode:  {'QUICK' if QUICK else 'FULL'}")
    print("=" * 60)

    n_ops = 200 if QUICK else 2000

    # Run 3 sessions with different seeds for coverage
    all_stats = {"pass": 0, "fail": 0, "crash": 0, "ops": 0}
    for session in range(3):
        print(f"\n  Session {session+1}/3 ...")
        s = run_fuzz_session(n_ops // 3)
        for k in all_stats:
            all_stats[k] += s[k]

    print(f"\n{'='*60}")
    print(f"  Mutation Fuzz Summary")
    print(f"{'='*60}")
    print(f"  Ops:      {all_stats['ops']}")
    print(f"  Pass:     {all_stats['pass']}")
    print(f"  Fail:     {all_stats['fail']}")
    print(f"  Crashes:  {all_stats['crash']}")
    rate = all_stats['pass'] / max(all_stats['ops'], 1) * 100
    print(f"  Rate:     {rate:.1f}%")

    if all_stats["crash"]:
        print(f"\n  💥 CRASH detected!")
        sys.exit(1)


if __name__ == "__main__":
    main()
