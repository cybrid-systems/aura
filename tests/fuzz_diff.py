#!/usr/bin/env python3
"""Differential Fuzz — run same program through tree-walk / IR / JIT, compare outputs.

Any difference between backends indicates a compiler bug.

Usage:
  python3 tests/fuzz_diff.py                    # full run
  python3 tests/fuzz_diff.py --quick            # 50 cases
  python3 tests/fuzz_diff.py --seed 42          # reproducible
"""

import datetime
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

# ── Code generators ──────────────────────────────────────
# Generate code tuples: (description, code)

GENERATORS = []

def register(fn):
    GENERATORS.append(fn)
    return fn


@register
def gen_arith():
    for _ in range(10 if not QUICK else 3):
        a, b = rng.randint(1, 100), rng.randint(1, 100)
        for op in ["+", "-", "*", "/", "quotient", "remainder"]:
            yield f"arith {a} {op} {b}", f"(display ({op} {a} {b}))"
        yield f"arith mixed", f"(display (+ (* 2 3) (- 10 4)))"


@register
def gen_compare():
    for _ in range(10 if not QUICK else 3):
        a, b = rng.randint(1, 100), rng.randint(1, 100)
        for op in ["=", "<", ">", "<=", ">="]:
            yield f"cmp {a} {op} {b}", f"(display ({op} {a} {b}))"


@register
def gen_define_call():
    for _ in range(10 if not QUICK else 3):
        fn = f"f{rng.randint(0, 100)}"
        val = rng.randint(1, 100)
        yield f"define+call", f"(define ({fn} x) (+ x {val})) (display ({fn} {rng.randint(0, 50)}))"


@register
def gen_if():
    for _ in range(10 if not QUICK else 3):
        cond = rng.choice(["#t", "#f", "(> 3 2)", "(< 1 0)"])
        a, b = rng.randint(1, 100), rng.randint(1, 100)
        yield f"if {cond}", f"(display (if {cond} {a} {b}))"


@register
def gen_begin_seq():
    yield "begin multi", "(begin (display 1) (display 2) (display 3))"
    yield "begin nested", "(begin (begin (display 1) (display 2)) (display 3))"


@register
def gen_lambda():
    yield "lambda apply", "(display ((lambda (x) (+ x 1)) 5))"
    yield "lambda closure", "(define (mk-adder n) (lambda (x) (+ x n))) (define add5 (mk-adder 5)) (display (add5 10))"


@register
def gen_let():
    for _ in range(10 if not QUICK else 3):
        a, b = rng.randint(1, 100), rng.randint(1, 100)
        yield f"let basic", f"(display (let ((x {a}) (y {b})) (+ x y)))"
    yield "let nested", "(display (let ((x 1)) (let ((y (+ x 1))) (+ x y))))"


@register
def gen_stdlib():
    """Stdlib functions — must produce same output on all backends."""
    yield "map basic", "(require std/list all:)(display (map (lambda (x) (+ x 1)) '(1 2 3)))"
    yield "filter basic", "(require std/list all:)(display (filter (lambda (x) (> x 2)) '(1 2 3 4 5)))"
    yield "length", "(require std/list all:)(display (length '(1 2 3 4 5)))"
    yield "reverse", "(require std/list all:)(display (reverse '(1 2 3)))"
    yield "cons pair", "(display (car (cons 1 2)))"
    yield "cdr pair", "(display (cdr (cons 1 2)))"


@register
def gen_from_tasks():
    """Sample some programs from the benchmark task suite."""
    import_path = REPO / "tests" / "tasks"
    if not import_path.exists():
        return
    # Sample 20 tasks
    tasks = sorted(import_path.rglob("*.aura"))
    for fpath in tasks[:40]:
        if fpath.stem == "README":
            continue
        text = fpath.read_text()
        # Extract goal
        goal = ""
        for line in text.splitlines():
            if line.startswith(";; goal:"):
                goal = line[len(";; goal:"):].strip()
                break
        # Also extract hints to make the program self-contained
        hints = []
        depend = ""
        for line in text.splitlines():
            if line.startswith(";; depend:"):
                depend = line[len(";; depend:"):].strip()
            elif line.startswith(";; hint:") and "CRITICAL" in line:
                hints.append(line)
        # Build a prompt-inspired program that would work
        name = fpath.stem
        if name in ("adt-either", "adt-tree", "adt-option"):
            continue  # skip ADT tasks, tree-walk doesn't support ADT
        yield name, goal[:100]


# ── Runner ───────────────────────────────────────────────
BACKENDS = [
    ("tree-walk", []),
    ("ir", ["--ir"]),
    ("jit", ["--jit"]),
]


def run(name, code):
    """Run code through all 3 backends, return results."""
    results = {}
    for bname, flags in BACKENDS:
        try:
            r = subprocess.run(
                [AURA] + flags,
                input=code,
                capture_output=True,
                text=True,
                timeout=10,
            )
            if r.returncode < 0:
                sig = -r.returncode
                sig_name = {6: "SIGABRT", 8: "SIGFPE", 11: "SIGSEGV"}.get(sig, f"signal-{sig}")
                results[bname] = (False, f"CRASH:{sig_name}", r.stderr)
            elif r.returncode != 0:
                results[bname] = (False, r.stdout.strip(), r.stderr.strip())
            else:
                results[bname] = (True, r.stdout.strip(), r.stderr.strip())
        except subprocess.TimeoutExpired:
            results[bname] = (False, "TIMEOUT", "")
        except FileNotFoundError:
            print(f"ERROR: {AURA} not found", file=sys.stderr)
            sys.exit(1)
    return results


def normalize(out):
    """Normalize output for comparison — strip extra () from IR."""
    # IR backend adds () for module result — strip trailing ()
    out = out.strip()
    while out.endswith("()"):
        out = out[:-2].strip()
    return out


def compare(diffs, name, results, code):
    """Compare results across backends. Return True if consistent."""
    # Get the tree-walker result as reference
    ref = results.get("tree-walk")
    if ref is None or not ref[0]:
        return False  # tree-walk itself failed

    ref_out = normalize(ref[1])

    consistent = True
    for bname in ["ir", "jit"]:
        if bname not in results:
            continue
        done, out, err = results[bname]
        if not done:
            diffs.append((name, f"{bname} CRASH", out, code[:100]))
            consistent = False
            continue
        normalized = normalize(out)
        if normalized != ref_out:
            diffs.append((name, f"{bname} DIFF",
                          f"tree-walk: {ref_out!r} vs {bname}: {normalized!r}",
                          code[:200]))
            consistent = False

    return consistent


# ── Main ────────────────────────────────────────────────
def main():
    print("=" * 60)
    print("Aura Differential Fuzz — tree-walk vs IR vs JIT")
    print(f"  Date:   {datetime.date.today().isoformat()}")
    print(f"  Seed:   {SEED if SEED is not None else 'random'}")
    print(f"  Binary: {AURA}")
    print(f"  Mode:   {'QUICK' if QUICK else 'FULL'}")
    print("=" * 60)

    total = 0
    passes = 0
    diffs = []  # (name, backend, detail, code)
    start = time.time()

    for gen_fn in GENERATORS:
        gname = gen_fn.__name__.replace("gen_", "")
        for name, code in gen_fn():
            total += 1
            results = run(name, code)
            if compare(diffs, name, results, code):
                passes += 1

        print(f"  {gname:20s} done", flush=True)

    elapsed = time.time() - start

    print(f"\n{'='*60}")
    print(f"  Differential Fuzz Summary")
    print(f"{'='*60}")
    print(f"  Cases:    {total}")
    print(f"  Pass:     {passes}")
    print(f"  Diffs:    {len(diffs)}")
    print(f"  Elapsed:  {elapsed:.1f}s")

    if diffs:
        print(f"\n  ❌ DIFFERENCES:")
        for name, backend, detail, code in diffs[:10]:
            print(f"    {name:30s} {backend:10s} {detail[:80]}")
        if len(diffs) > 10:
            print(f"    ... and {len(diffs) - 10} more")
        sys.exit(1)


if __name__ == "__main__":
    main()
