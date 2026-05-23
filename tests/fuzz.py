#!/usr/bin/env python3
"""Aura Compiler Fuzz Tests — random/structure/edge fuzzing, no LLM.

Usage:
  python3 tests/fuzz.py                    # full run
  python3 tests/fuzz.py --quick            # fast subset
  python3 tests/fuzz.py --seed 42          # reproducible
  python3 tests/fuzz.py --list             # list test groups

Architecture:
  - generator/          produces code strings
  - runner/             executes via aura binary, checks for crashes
  - reporter/           collects results, writes reproducers

Tests are organized by "fuzz dimension":
  1. random-tokens     — parser-agnostic random byte sequences
  2. random-sexpr      — structurally valid but semantically random s-exprs
  3. nesting           — deeply nested expressions (parser stack depth)
  4. large-const       — oversized literals (big ints, long strings)
  5. unicode           — Unicode identifiers and string content
  6. struct-mutate     — mutate valid code: delete/swap/duplicate nodes
  7. recursion         — (lambda (x) (x x)) style pathological cases
  8. binding           — shadowing cycles, mutual recursion edge cases
  9. arena             — large module load/unload cycles
  10. serve-protocol   — malformed JSON commands to --serve

Output:
  - Summary to stdout
  - Crashes/timeouts written to tests/reproducers/fuzz_YYYY-MM-DD/
  - Returns non-zero on any crash
"""

import datetime
import os
import random
import signal
import string
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
AURA = os.environ.get("AURA_BIN", str(REPO / "build" / "aura"))
REPRO_DIR = HERE / "reproducers" / f"fuzz_{datetime.date.today().isoformat()}"
TIMEOUT = int(os.environ.get("FUZZ_TIMEOUT", "5"))  # per-test timeout seconds
QUICK = "--quick" in sys.argv
SEED = None
for i, a in enumerate(sys.argv):
    if a == "--seed" and i + 1 < len(sys.argv):
        SEED = int(sys.argv[i + 1])
if "--list" in sys.argv:
    print("Fuzz dimensions:")
    for d in DIMENSIONS:
        print(f"  {d['name']:20s} {d['desc']}")
    sys.exit(0)

rng = random.Random(SEED if SEED is not None else None)

# ── Results ────────────────────────────────────────────────
results = {
    "pass": 0,
    "fail": 0,
    "crash": [],
    "timeout": [],
}

saw_signal = False


def run(code, timeout=TIMEOUT):
    """Run code through Aura. Returns (ok, stdout, stderr) or raises on crash."""
    global saw_signal
    try:
        r = subprocess.run(
            [AURA],
            input=code,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        results["timeout"].append(code[:80])
        return False, "", "timeout"
    except FileNotFoundError:
        print("  ERROR: aura binary not found at", AURA, file=sys.stderr)
        sys.exit(1)

    if r.returncode < 0:
        sig = -r.returncode
        sig_name = {6: "SIGABRT", 8: "SIGFPE", 11: "SIGSEGV"}.get(sig, f"signal-{sig}")
        results["crash"].append((sig_name, code))
        saw_signal = True
        return False, "", sig_name

    stderr = r.stderr or ""
    # Graceful depth errors are not crashes
    if "recursion depth exceeded" in stderr:
        results["fail"] += 1
        return ok, (r.stdout or "").strip(), stderr.strip()
    if "internal error" in stderr or "Assertion" in stderr:
        results["crash"].append(("internal-error", code))
        return False, "", "internal error"

    ok = r.returncode == 0
    return ok, (r.stdout or "").strip(), stderr.strip()


# ── Generators ─────────────────────────────────────────────
GENERATORS = []


def register(fn):
    GENERATORS.append(fn)
    return fn


# ── 1. Random token sequences ──────────────────────────
TOKENS = [
    "(", ")", "'", "`", ",", ",@",
    '"', '"',        # mismatched quotes
    ";", "#", "|", "[", "]", "{", "}",
    "+", "-", "*", "/", "=", "<", ">",
    "0", "1", "42", "-1", "3.14",
    "define", "lambda", "let", "if", "cond",
    "display", "+", "car", "cdr", "cons",
    "begin", "quote", "set!", "and", "or",
    "match", "datatype", "require", "import",
    ":",
    "\x00", "\x01", "\x1f", "\x7f",  # control chars
]


@register
def gen_random_tokens():
    """Generate N random sequences of random tokens — pure parser torture."""
    n = 500 if not QUICK else 50
    for _ in range(n):
        length = rng.randint(1, 40)
        pieces = [rng.choice(TOKENS) for _ in range(length)]
        # Occasionally add valid-ish structure
        if rng.random() < 0.3:
            depth = rng.randint(1, 5)
            pieces = ["("] * depth + pieces + [")"] * depth
        code = " ".join(pieces)
        yield code


# ── 2. Random structurally valid s-expressions ────────
SEXPR_ATOMS = [
    "0", "1", "42", "-1", "3.14", "#t", "#f",
    "x", "y", "f", "g", "nil", "+", "foo", "bar",
]


def random_sexpr(depth=0):
    if depth > 6 or (depth > 0 and rng.random() < 0.4):
        return rng.choice(SEXPR_ATOMS)
    head = rng.choice(SEXPR_ATOMS)
    nargs = rng.randint(0, 4)
    args = " ".join(random_sexpr(depth + 1) for _ in range(nargs))
    return f"({head} {args})" if args else f"({head})"


@register
def gen_random_sexpr():
    """Generate structurally valid random s-expressions. Tests tree-walker."""
    n = 300 if not QUICK else 30
    for _ in range(n):
        code = random_sexpr(0)
        # Sometimes wrap in display
        if rng.random() < 0.3:
            code = f"(display {code})"
        yield code


# ── 3. Deep nesting — parser stack depth ──────────────
@register
def gen_deep_nesting():
    """Deeply nested expressions to stress parser recursion depth."""
    for depth in [10, 50, 100, 200, 500, 1000, 2000]:
        if QUICK and depth > 200:
            continue
        # Nesting via (+ (+ (+ ... )))
        code = "0"
        for _ in range(depth):
            code = f"(+ {code} 1)"
        code = f"(display {code})"
        yield code

        # Nesting via lists
        code = "x"
        for _ in range(min(depth, 100)):
            code = f"(car {code})"
        yield f"(display {code})"

        # Deep lambda binding
        code = "x"
        for _ in range(min(depth, 50)):
            code = f"((lambda (x) {code}) 1)"
        yield f"(display {code})"


# ── 4. Large constants ────────────────────────────────
@register
def gen_large_constants():
    """Oversized integers, long strings, huge lists — memory/alloc stress."""
    for bits in [64, 128, 256, 512, 1024, 2048, 4096]:
        if QUICK and bits > 1024:
            continue
        big_int = 2**bits - 1
        yield f"(display {big_int})"
        yield f"(display (- {big_int}))"

    # Large string
    for size in [100, 1000, 10000, 50000]:
        if QUICK and size > 10000:
            continue
        big_str = '"' + "x" * size + '"'
        yield f"(display {big_str})"

    # Huge list literal
    for size in [10, 100, 1000, 5000]:
        if QUICK and size > 1000:
            continue
        huge = "(" + " ".join(str(rng.randint(0, 100)) for _ in range(size)) + ")"
        yield f"(display (length {huge}))"


# ── 5. Unicode ────────────────────────────────────────
@register
def gen_unicode():
    """Unicode identifiers and string content — encoding/decoding stress."""
    symbols = ["λ", "α", "ω", "∑", "€", "中", "文", "日本語", "🌵", "🔥", "\\u0000"]
    n = 30 if not QUICK else 10
    for _ in range(n):
        name = "".join(rng.choice(symbols) for _ in range(rng.randint(1, 4)))
        code = f"(display {name})"
        yield code
        code = f"(define {name} 42)(display {name})"
        yield code

    # Unicode strings
    for s in ["héllo", "你好", "日本語", "🌵🔥"]:
        yield f'(display "{s}")'


# ── 6. Structural mutation ────────────────────────────
VALID_BASES = [
    "(display (+ 1 2))",
    "(define (f x) (* x 2)) (display (f 5))",
    "(let ((x 1) (y 2)) (display (+ x y)))",
    "(if #t (display 1) (display 0))",
    "(begin (define x 10) (display x))",
    '((lambda (x) (display x)) 42)',
    "(display (car '(1 2 3)))",
    "(display (cons 1 '(2 3)))",
]


def mutate(code):
    """Apply a random mutation to valid code."""
    chars = list(code)
    op = rng.choice(["delete", "swap", "duplicate", "insert"])
    if op == "delete" and len(chars) > 10:
        start = rng.randint(0, len(chars) - 1)
        end = min(start + rng.randint(1, 3), len(chars))
        del chars[start:end]
    elif op == "swap" and len(chars) > 4:
        i, j = rng.sample(range(len(chars)), 2)
        chars[i], chars[j] = chars[j], chars[i]
    elif op == "duplicate" and len(chars) > 2:
        i = rng.randint(0, len(chars) - 2)
        chars.insert(i + 1, chars[i])
    elif op == "insert":
        i = rng.randint(0, len(chars))
        chars.insert(i, rng.choice(TOKENS))
    return "".join(chars)


@register
def gen_struct_mutate():
    """Mutate known-valid code — test compiler robustness on near-valid input."""
    n = 200 if not QUICK else 30
    for _ in range(n):
        base = rng.choice(VALID_BASES)
        code = mutate(base)
        yield code


# ── 7. Pathological recursion ────────────────────────
@register
def gen_pathological():
    """Self-referential / infinite-loop / pathological cases."""
    cases = [
        # Self-calling lambda
        "(display ((lambda (x) (x x)) (lambda (x) (x x))))",
        # Infinite recursion via define
        "(define (inf) (inf)) (display 1)",
        # Mutual recursion
        "(define (a) (b)) (define (b) (a)) (display 1)",
        # Overflow via recursion
        "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) (display (fact 100000))",
        # Huge let binding chain
        "(" + " ".join(f"((lambda (x) {{}})" for _ in range(500)) + " 1" + ")" * 500,
        # Huge lambda nesting
        "(display " + "".join(f"(lambda (x{i}) " for i in range(500)) + "1" + ")" * 500,
        # Quasiquote depth
        "`" * 50 + "(1 2 3)",
    ]
    for c in cases:
        yield c


# ── 8. Binding edge cases ────────────────────────────
@register
def gen_binding_edge():
    """Edge cases in variable binding: shadowing, cycles, weird scopes."""
    n = 50 if not QUICK else 10
    for _ in range(n):
        depth = rng.randint(2, 8)
        # Nested lets with shadowing
        lets = []
        shadow_vars = set()
        for i in range(depth):
            if rng.random() < 0.3 and shadow_vars:
                var = rng.choice(list(shadow_vars))
            else:
                var = chr(ord("a") + i % 26)
                shadow_vars.add(var)
            val = rng.randint(0, 100)
            lets.append(f"({var} {val})")
        body = " ".join(rng.choice(list(shadow_vars | {"1"})) for _ in range(3))
        code = f"(let ({' '.join(lets)}) (display {body}))"
        yield code


# ── 9. Arena / memory pressure ───────────────────────
@register
def gen_arena_pressure():
    """Repeated module load/unload, large AST builds — arena allocator stress."""
    n = 50 if not QUICK else 10
    for _ in range(n):
        # Repeated define/eval in same session
        lines = []
        for i in range(500 if not QUICK else 100):
            op = rng.choice(["define", "set-code", "display"])
            if op == "define":
                fn = f"f{i}"
                lines.append(f"(define ({fn} x) (+ x {rng.randint(1, 100)}))")
                lines.append(f"(display ({fn} {rng.randint(0, 1000)}))")
            elif op == "set-code":
                lines.append(f'(set-code "(display {rng.randint(0, 100)})")')
            else:
                lines.append(f"(display {rng.randint(0, 1000)})")
        code = "(begin " + " ".join(lines) + ")"
        yield code


# ── 10. Serve protocol fuzz ──────────────────────────
@register
def gen_serve_fuzz():
    """Feed malformed JSON to --serve. Tests serve protocol robustness."""
    n = 30 if not QUICK else 10
    for _ in range(n):
        length = rng.randint(5, 200)
        bad_json = "".join(rng.choice(string.printable) for _ in range(length))
        yield bad_json


# ── Runner ────────────────────────────────────────────────
def run_serve_fuzz(code):
    """Special runner for serve protocol fuzz (send to --serve stdin)."""
    try:
        r = subprocess.run(
            [AURA, "--serve"],
            input=code + "\n",
            capture_output=True,
            text=True,
            timeout=3,
        )
    except subprocess.TimeoutExpired:
        return
    if r.returncode < 0:
        sig = -r.returncode
        sig_name = {6: "SIGABRT", 8: "SIGFPE", 11: "SIGSEGV"}.get(sig, f"signal-{sig}")
        results["crash"].append((sig_name, f"[serve] {code[:60]}"))


def run_group(name, gen_fn):
    """Run one fuzz dimension, reporting results."""
    total = 0
    crashes_before = len(results["crash"])
    start = time.time()

    for code in gen_fn():
        total += 1

        if name == "serve-protocol":
            run_serve_fuzz(code)
        else:
            ok, out, err = run(code)
            if ok:
                results["pass"] += 1
            else:
                results["fail"] += 1

        if total % 100 == 0:
            elapsed = time.time() - start
            eta = (elapsed / total) * (total + 1) if total else 0
            print(
                f"    {name:20s} {total:5d} cases  "
                f"[{len(results['crash'])} crashes, {results['fail']} fails]"
                f"  ({elapsed:.0f}s)",
                flush=True,
            )

    elapsed = time.time() - start
    new_crashes = len(results["crash"]) - crashes_before
    status = "💥 CRASH" if new_crashes else "✅"
    print(
        f"  {status} {name:20s} {total:5d} cases  "
        f"{elapsed:.1f}s  ({new_crashes} crashes)",
        flush=True,
    )


# ── Main ─────────────────────────────────────────────────
def main():
    DIMENSIONS = [
        ("random-tokens", gen_random_tokens, "Parser: random token sequences"),
        ("random-sexpr", gen_random_sexpr, "Parser: random s-exprs"),
        ("deep-nesting", gen_deep_nesting, "Parser: deep expression nesting"),
        ("large-const", gen_large_constants, "Allocator: large literals"),
        ("unicode", gen_unicode, "Encoding: Unicode identifiers"),
        ("struct-mutate", gen_struct_mutate, "Parser: mutated valid code"),
        ("pathological", gen_pathological, "Semantics: pathological cases"),
        ("binding-edge", gen_binding_edge, "Binding: shadowing/cycles"),
        ("arena-pressure", gen_arena_pressure, "Arena: repeated alloc/free"),
        ("serve-fuzz", gen_serve_fuzz, "Protocol: malformed JSON"),
    ]

    print(f"=" * 60)
    print(f"Aura Fuzz Suite")
    print(f"  Date:    {datetime.date.today().isoformat()}")
    print(f"  Seed:    {SEED if SEED is not None else 'random'}")
    print(f"  Binary:  {AURA}")
    print(f"  Timeout: {TIMEOUT}s per case")
    print(f"  Mode:    {'QUICK' if QUICK else 'FULL'}")
    print(f"=" * 60)

    total_start = time.time()

    for name, gen_fn, desc in DIMENSIONS:
        print(f"\n{'─'*60}")
        print(f"  {name}")
        print(f"  {desc}")
        print(f"{'─'*60}")
        run_group(name, gen_fn)

    # Summary
    elapsed = time.time() - total_start
    total = results["pass"] + results["fail"]
    print(f"\n{'='*60}")
    print(f"  Fuzz Summary")
    print(f"{'='*60}")
    print(f"  Total cases:  {total}")
    print(f"  Pass:         {results['pass']}")
    print(f"  Fail:         {results['fail']}")
    print(f"  Crashes:      {len(results['crash'])}")
    print(f"  Timeouts:     {len(results['timeout'])}")
    print(f"  Elapsed:      {elapsed:.1f}s")

    if results["crash"]:
        print(f"\n  💥 CRASHES:")
        for sig, code in results["crash"][:10]:
            print(f"    {sig:15s} {code[:80]}")
        if len(results["crash"]) > 10:
            print(f"    ... and {len(results['crash']) - 10} more")

    # Write reproducers
    if results["crash"]:
        REPRO_DIR.mkdir(parents=True, exist_ok=True)
        for i, (sig, code) in enumerate(results["crash"]):
            fname = f"{datetime.datetime.now().strftime('%H%M%S')}_{i:03d}_{sig}.aura"
            (REPRO_DIR / fname).write_text(
                f";; fuzz reproducer: {sig}\n"
                f";; seed: {SEED}\n"
                f";; time: {datetime.datetime.now().isoformat()}\n"
                f";; expect: no-crash\n"
                f"{code}\n"
            )
        print(f"\n  Reproducers: {REPRO_DIR}/")

    # Return code
    if results["crash"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
