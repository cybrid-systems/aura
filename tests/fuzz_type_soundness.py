#!/usr/bin/env python3
"""Type Soundness Fuzz — verify type invariants survive arbitrary mutation sequences.

Strategy: start with a well-typed program, apply random mutation operations through
--serve, and after each mutation verify:
  1. typecheck-current reports "no errors"
  2. eval-current produces a result (no runtime crash)

Guards against issue #25: type system unsoundness in self-modification scenarios.

Usage:
  python3 tests/fuzz_type_soundness.py               # 500 ops
  python3 tests/fuzz_type_soundness.py --quick       # 100 ops
  python3 tests/fuzz_type_soundness.py --seed 42     # reproducible
"""

import json, os, random, subprocess, sys, time
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

# Well-typed seed programs — each defines several functions with different types
SEED_PROGRAMS = [
    # Arithmetic functions
    """
(define (add x y) (+ x y))
(define (sub x y) (- x y))
(define (mul x y) (* x y))
(define (div x y) (/ x y))
(define (inc x) (+ x 1))
(define (dec x) (- x 1))
(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))
(display (add 10 20))
""",
    # String + number
    """
(define (greet name) (string-append "Hello, " name))
(define (len s) (string-length s))
(define (add x y) (+ x y))
(define (pair-add p) (+ (car p) (cdr p)))
(display (add 1 2))
""",
    # Higher-order functions
    """
(define (map f lst) (if (null? lst) '() (cons (f (car lst)) (map f (cdr lst)))))
(define (filter pred lst) (if (null? lst) '() (if (pred (car lst)) (cons (car lst) (filter pred (cdr lst))) (filter pred (cdr lst)))))
(define (inc x) (+ x 1))
(define (even? x) (= (modulo x 2) 0))
(display (map inc '(1 2 3)))
""",
    # Type-annotated
    """
(: add Int Int -> Int)
(define (add x y) (+ x y))
(: greet String -> String)
(define (greet name) (string-append "Hello, " name))
(display (add 1 2))
""",
]

MUTATIONS = [
    "mutate:rebind",
    "mutate:set-body",
    "mutate:wrap",
    "mutate:splice",
    "mutate:tweak-literal",
    "mutate:extract-function",
    "mutate:inline-call",
]

# ── Helper: build mutation commands ──────────────────────────────
def pick_fn(existing_fns):
    return rng.choice(existing_fns)

def pick_op():
    return rng.choice(["+", "-", "*", "car", "cdr", "string-length", "not"])

def pick_val():
    return rng.randint(0, 100)

def build_mutation(fns, mut_type, serve_script):
    """Build mutation command(s) for a given mutation type.
    Appends necessary query commands to serve_script for operations
    that need dynamic node IDs."""
    fn = pick_fn(fns) if fns else "add"
    if mut_type == "mutate:rebind":
        op = pick_op()
        val = pick_val()
        return [f'(mutate:rebind "{fn}" "(lambda (x) ({op} x {val}))")']
    elif mut_type == "mutate:set-body":
        return [f'(mutate:set-body "{fn}" "(display 42)")']
    elif mut_type == "mutate:wrap":
        return [f'(mutate:wrap 0 "display")']
    elif mut_type == "mutate:splice":
        return [f'(mutate:splice 1 1)']
    elif mut_type == "mutate:tweak-literal":
        return [f'(mutate:tweak-literal 0 {pick_val()})']
    elif mut_type == "mutate:extract-function":
        # Query for Define nodes, then try extract-function on the first one.
        # The node pointed to by query:find is the Define node ID.
        # extract-function takes the BODY expression node, which for
        # (define (name ...) body) is at Define.child[0].child[1] (Lambda's body).
        # We store query results for the next iteration to use.
        return [
            f'(query:find "{fn}")',
            f'(mutate:extract-function 2 "extracted_{rng.randint(100,999)}")',
        ]
    elif mut_type == "mutate:inline-call":
        # Query and try inline-call. Use query first to check if node exists.
        return [
            f'(query:node-type "Call")',
            '(mutate:inline-call 3)',
        ]
    return [f'(mutate:rebind "{fn}" "(lambda (x) (+ x 1))")']

def parse_serve_response(raw):
    """Parse multi-line JSON response from --serve-async."""
    lines = raw.strip().split("\n")
    for line in lines:
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                continue
    return None

def run_test(num_ops):
    print(f"Type Soundness Fuzz (seed={SEED}, ops={num_ops})")
    print()

    seed_prog = rng.choice(SEED_PROGRAMS)
    serve_script = []

    # Step 1: set-code with the seed program
    serve_script.append(f'(workspace:set-code "{seed_prog.strip()}")')
    serve_script.append("(typecheck-current)")
    serve_script.append("(eval-current)")

    # Parse function names from seed program
    import re as _re
    existing_fns = _re.findall(r'\(define \(([\w?-]+)', seed_prog)
    if not existing_fns:
        existing_fns = ["add"]

    # Step 2: apply random mutations
    for i in range(num_ops):
        mut = rng.choice(MUTATIONS)
        cmds = build_mutation(existing_fns, mut, serve_script)
        for cmd in cmds:
            serve_script.append(cmd)
        serve_script.append("(typecheck-current)")
        serve_script.append("(eval-current)")

        # Occasionally add a new function
        if rng.random() < 0.1:
            new_name = f"fn_{i}"
            existing_fns.append(new_name)
            serve_script.append(f'(workspace:set-code \'(define ({new_name} x) (+ x 1))\')')
            serve_script.append("(typecheck-current)")
            serve_script.append("(eval-current)")

    # Step 3: run via --serve-async
    full_input = "\n".join(serve_script)
    proc = subprocess.run(
        [AURA, "--serve-async"],
        input=full_input,
        capture_output=True,
        text=True,
        timeout=60
    )

    out = proc.stdout
    err = proc.stderr

    # Parse responses
    type_errors = 0
    eval_failures = 0
    total_ops = num_ops + 1  # +1 for initial

    for line in out.split("\n"):
        line = line.strip()
        if not line:
            continue
        resp = parse_serve_response(line)
        if resp is None:
            continue

        # Check for type errors in typecheck-current responses
        if "no errors" not in str(resp).lower():
            # Could be a type error — check if it's an error response
            if isinstance(resp, dict) and "typecheck" in str(resp).lower():
                type_errors += 1
            elif isinstance(resp, dict) and "error" in resp:
                eval_failures += 1

    # Print results
    print(f"  Operations:     {total_ops}")
    print(f"  Type errors:    {type_errors}")
    print(f"  Eval failures:  {eval_failures}")

    if type_errors > 0 or eval_failures > 0:
        print(f"\n  ❌ FAILED: {type_errors} type errors, {eval_failures} eval failures")
        # Print last few responses for debugging
        print(f"\n  Last 5 lines of output:")
        for line in out.split("\n")[-5:]:
            print(f"    {line.strip()}")
        return False

    print(f"\n  ✅ PASSED: 0 type errors, 0 eval failures")
    return True

if __name__ == "__main__":
    num_ops = 100 if QUICK else 500
    ok = run_test(num_ops)
    sys.exit(0 if ok else 1)
