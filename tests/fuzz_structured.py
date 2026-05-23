#!/usr/bin/env python3
"""Aura Structured Fuzz — stdlib, type system, incremental compilation, modules, EDSL.

Tests specific Aura language features with edge-case inputs.
No LLM dependency — all generated algorithmically.

Usage:
  python3 tests/fuzz_structured.py                    # full run
  python3 tests/fuzz_structured.py --quick            # fast subset
  python3 tests/fuzz_structured.py --seed 42          # reproducible
  python3 tests/fuzz_structured.py --list             # list dimensions
"""

import datetime
import os
import random
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
AURA = os.environ.get("AURA_BIN", str(REPO / "build" / "aura"))
TIMEOUT = int(os.environ.get("FUZZ_TIMEOUT", "5"))
QUICK = "--quick" in sys.argv
SEED = None
for i, a in enumerate(sys.argv):
    if a == "--seed" and i + 1 < len(sys.argv):
        SEED = int(sys.argv[i + 1])
if "--list" in sys.argv:
    print("Structured fuzz dimensions:")
    for d in DIMENSIONS:
        print(f"  {d['name']:25s} {d['desc']}")
    sys.exit(0)

rng = random.Random(SEED if SEED is not None else None)

results = {"pass": 0, "fail": 0, "crash": [], "timeout": []}


def run(code, timeout=TIMEOUT):
    """Run code through Aura. Returns (ok, stdout, stderr) on success."""
    try:
        r = subprocess.run(
            [AURA], input=code, capture_output=True, text=True, timeout=timeout
        )
        ok = r.returncode == 0
    except subprocess.TimeoutExpired:
        results["timeout"].append(code[:80])
        return False, "", "timeout"
    except FileNotFoundError:
        print(f"  ERROR: aura binary not found at {AURA}", file=sys.stderr)
        sys.exit(1)

    if r.returncode < 0:
        sig = -r.returncode
        sig_name = {6: "SIGABRT", 8: "SIGFPE", 11: "SIGSEGV"}.get(sig, f"signal-{sig}")
        results["crash"].append((sig_name, code))
        return False, "", sig_name

    stderr = r.stderr or ""
    # Graceful depth/out-of-memory errors are not crashes
    if "recursion depth exceeded" in stderr:
        results["fail"] += 1
        return False, (r.stdout or "").strip(), stderr.strip()
    if "internal error" in stderr or "Assertion" in stderr:
        results["crash"].append(("internal-error", code))
        return False, "", "internal error"

    return ok, (r.stdout or "").strip(), stderr.strip()


REGISTER = []


def register(fn):
    REGISTER.append(fn)
    return fn


# ═══════════════════════════════════════════════════════════
# 1. Std Lib — call every function with edge case arguments
# ═══════════════════════════════════════════════════════════
STDLIB_MODULES = {
    "std/list": ["map", "filter", "foldl", "foldr", "range", "take", "drop",
                  "length", "reverse", "append", "zip", "flatten", "partition"],
    "std/string": ["string-index", "string-contains?", "string-downcase", "string-trim"],
    "std/pair": ["car", "cdr", "cons", "pair?", "list"],
    "std/hash": ["hash-set!", "hash-ref", "hash-has-key?", "hash-keys", "make-hash"],
}

@register
def gen_stdlib_edge():
    """Call stdlib functions with edge-case arguments (empty lists, None, large values)."""
    for mod, fns in STDLIB_MODULES.items():
        for fn in fns[:3]:  # 3 per module
            yield f"(require {mod} all:)(display ({fn} '()))"
            yield f"(require {mod} all:)(display ({fn} 0))"
            yield f"(require {mod} all:)(display ({fn} -1))"
    # Pair with non-pairs
    yield "(car '())"
    yield "(cdr '())"
    yield "(car 42)"
    yield "(cdr \"hello\")"
    # List operations on non-lists
    yield "(require std/list all:)(display (map (lambda (x) x) 42))"


# ═══════════════════════════════════════════════════════════
# 2. Type System — type annotations, gradual typing, let-poly
# ═══════════════════════════════════════════════════════════
@register
def gen_type_system():
    """Type annotation edge cases: all valid types, unbound, self-referential."""
    types = ["Int", "Float", "String", "Bool", "Void", "Dyn",
             "(List Int)", "(List String)", "(-> Int Int)", "(-> Dyn Dyn)"]
    for t in types:
        yield f"(: x {t}) (define x 42) (display x)"
        yield f"(: x {t}) (define x \"hello\") (display x)"
        yield f"(: x {t}) (define x #t) (display x)"

    # Let-polymorphism edge
    for t in types[:4]:
        for val in ["42", "\"hi\"", "#t"]:
            yield f"(: x {t}) (define x {val}) (display x)"

    # Type boundary calls
    yield "(display (+ 1 2.5))"
    yield "(display (+ 2.5 1))"
    yield '(display (string-append "a" "b"))'
    yield "(display (= 1 1.0))"
    yield '(display (= "a" "b"))'


# ═══════════════════════════════════════════════════════════
# 3. ADT / Datatype — match exhaustiveness, edge cases
# ═══════════════════════════════════════════════════════════
@register
def gen_adt():
    """Datatype definitions and match — recursive types, empty variants."""
    # Recursive type
    yield ("(require std/pair all:)"
           "(datatype (List : T) (Nil) (Cons T (List T)))"
           "(display (match (Cons 1 (Nil)) ((Nil) 0) ((Cons h t) h)))")

    # Multi-constructor edge
    yield ("(datatype (Maybe : T) (None) (Some T))"
           "(display (match (None) ((None) 0) ((Some x) x)))")

    # Match non-exhaustive (this is valid — else clause catches)
    yield ("(datatype (E : T) (A T) (B T))"
           "(display (match (A 1) ((A x) x) (else 0)))")

    # ADT in type annotation
    yield ("(require std/pair all:)"
           "(datatype (Wrap : T) (Wrap T))"
           "(: x (Wrap Int))"
           "(define x (Wrap 42))"
           "(display (match x ((Wrap v) v)))")


# ═══════════════════════════════════════════════════════════
# 4. M4 Linear Ownership — borrow/move/drop sequences
# ═══════════════════════════════════════════════════════════
@register
def gen_m4_linear():
    """Linear ownership edge cases: double-move, use-after-move, borrow-before-move."""
    # Double move
    yield "(linear:move x) (linear:move x)"
    # Use after move
    yield "(define x 42) (linear:move x) (display x)"
    # Borrow then move
    yield "(define x 42) (linear:borrow x) (linear:move x)"
    # Drop and use
    yield "(define x 42) (linear:drop x) (display x)"
    # Chain
    yield ("(define x 42) "
           "(define y (linear:move x)) "
           "(display y)")
    # Mut-borrow
    yield ("(define x (hash 42)) "
           "(linear:mut-borrow x) "
           "(display x)")


# ═══════════════════════════════════════════════════════════
# 5. Incremental Compilation — serve redefine/dep tracking
# ═══════════════════════════════════════════════════════════
@register
def gen_incremental():
    """Serve protocol: redefine functions, check dependency invalidation."""
    n = 30 if not QUICK else 10
    for _ in range(n):
        fn = f"f{rng.randint(0, 100)}"
        val = rng.randint(0, 100)
        # Redefine sequence
        yield (f'{{"cmd":"exec","code":"(define ({fn} x) (+ x {val}))"}}\n'
               f'{{"cmd":"exec","code":"(display ({fn} {val}))"}}\n'
               f'{{"cmd":"redefine","code":"(define ({fn} x) (* x {val}))"}}\n'
               f'{{"cmd":"exec","code":"(display ({fn} {val}))"}}')
    # Dependency chain: f1 → f2 → f3
    yield ('{"cmd":"exec","code":"(define (f3 x) (* x 3))"}\n'
           '{"cmd":"exec","code":"(define (f2 x) (f3 (+ x 1)))"}\n'
           '{"cmd":"exec","code":"(define (f1 x) (f2 x))"}\n'
           '{"cmd":"exec","code":"(display (f1 5))"}\n'
           '{"cmd":"redefine","code":"(define (f3 x) (* x 10))"}\n'
           '{"cmd":"exec","code":"(display (f1 5))"}')
    # Circular dependency (should error gracefully)
    yield ('{"cmd":"exec","code":"(define (a x) (b x))"}\n'
           '{"cmd":"exec","code":"(define (b x) (a x))"}')


# ═══════════════════════════════════════════════════════════
# 6. Module System — require, load/unload, cross-module
# ═══════════════════════════════════════════════════════════
@register
def gen_module():
    """Module load/unload edge cases: multiple require, circular."""
    for mod in ["std/list", "std/string", "std/pair", "std/hash"]:
        yield f"(require {mod} all:)(display 1)"
        yield f"(use \"{mod}\")"
        # Double require (should be idempotent)
        yield f"(require {mod} all:)(require {mod} all:)(display 1)"
        # Selective import
        yield f"(require {mod} map filter)(display 1)"


# ═══════════════════════════════════════════════════════════
# 7. EDSL — query/mutate/set-code/eval-current chains
# ═══════════════════════════════════════════════════════════
@register
def gen_edsl():
    """EDSL query/mutate/set-code sequences: empty AST, chain operations."""
    # set-code then query
    yield '(set-code "(define (f x) (+ x 1))")(display (query:node-type "Define"))'
    yield '(set-code "(define (f x) (+ x 1))")(display (query:find "f"))'
    yield '(set-code "(define (f x) (+ x 1))")(display (query:calls "f"))'
    yield '(set-code "(define (f x) (+ x 1))")(display (current-source))'
    # mutate then eval
    yield ('(set-code "(define (f x) (+ x 1))")'
           '(mutate:rebind "f" "(lambda (x) (* x 2))")'
           '(eval-current)')
    # set-code on empty/nil
    yield '(set-code "")(display (current-source))'
    yield '(set-code "()")(display (current-source))'
    # Query on empty AST
    yield '(display (query:node-type "Define"))'
    yield '(display (query:find "nonexistent"))'
    # Chain: set-code → mutate → query
    yield ('(set-code "(define (f x) (+ x 1))")'
           '(mutate:rebind "f" "(lambda (x) (* x 10))")'
           '(display (query:node-type "Lambda"))')
    # Remove node
    yield ('(set-code "(begin (define x 1) (define y 2))")'
           '(display (query:children 0))')


# ═══════════════════════════════════════════════════════════
# 8. Hash Tables — edge cases in hash operations
# ═══════════════════════════════════════════════════════════
@register
def gen_hash_edge():
    """Hash table edge cases: missing keys, nested, large."""
    yield "(require std/hash all:)(define h (hash))(display (hash-ref h 42))"
    yield "(require std/hash all:)(define h (hash 1 \"a\" 2 \"b\"))(display (hash-keys h))"
    yield "(require std/hash all:)(define h (hash))(hash-set! h 1 'a)(hash-set! h 1 'b)(display (hash-ref h 1))"
    # Hash of hash
    yield ("(require std/hash all:)(define inner (hash 'k 1))"
           "(define outer (hash 'inner inner))(display (hash-ref (hash-ref outer 'inner) 'k))")
    # Empty hash operations
    yield "(require std/hash all:)(define h (hash))(display (hash-has-key? h 99))"
    yield "(require std/hash all:)(define h (hash))(hash-set! h 'x 42)(hash-set! h 'x 99)(display (hash-ref h 'x))"


# ═══════════════════════════════════════════════════════════
# 9. Recursion edge cases — tail recursion, mutual recursion
# ═══════════════════════════════════════════════════════════
@register
def gen_recursion_edge():
    """Recursion edge cases: TCO, mutual recursion, deep but finite."""
    # Tail-recursive sum
    yield ("(define (sum n acc) (if (= n 0) acc (sum (- n 1) (+ acc n))))"
           "(display (sum 10000 0))")
    # Mutual recursion (limited depth)
    yield ("(define (even? n) (if (= n 0) #t (odd? (- n 1))))"
           "(define (odd? n) (if (= n 0) #f (even? (- n 1))))"
           "(display (even? 100))")
    # Ackermann (small values — huge growth)
    yield ("(define (ack m n) (if (= m 0) (+ n 1) (if (= n 0) (ack (- m 1) 1) (ack (- m 1) (ack m (- n 1))))))"
           "(display (ack 3 4))")


# ═══════════════════════════════════════════════════════════
# 10. Coercion / Blame — type boundary edge cases
# ═══════════════════════════════════════════════════════════
@register
def gen_coercion():
    """Type coercion edge cases: Int→Float, String→Int, blame output."""
    yield "(display (+ 1 2.5))"
    yield "(display (+ 2.5 1))"
    yield "(display (+ 1 2.0))"
    yield '(display (= 42 "hello"))'
    yield "(display (= 1.5 1))"
    # Blame: wrong type at boundary
    yield "(display ((: (lambda (x) (+ x 1)) (-> Int Int)) \"hello\"))"
    yield "(display ((: (lambda (x) (string-length x)) (-> String Int)) 42))"


# ═══════════════════════════════════════════════════════════
# 11. Serve Protocol — multi-session, timeout, malformed
# ═══════════════════════════════════════════════════════════
@register
def gen_serve_structured():
    """Serve protocol: multi-session, session switch, malformed JSON."""
    n = 20 if not QUICK else 5
    for _ in range(n):
        yield random.choice([
            '{"cmd":"session","name":"new:test"}',
            '{"cmd":"exec","code":"(display 42)"}',
            '{"cmd":"invalid"}',
            '{"cmd":"exec","code":""}',
            'not json at all',
            '{broken json',
            '{"cmd":"mutate","op":"nonexistent","node":0}',
        ])



# ═══════════════════════════════════════════════════════════
# 13. Arrow / Higher-Order Functions — map/filter/compose edge
# ═══════════════════════════════════════════════════════════
@register
def gen_higher_order():
    """Higher-order function edge cases: currying, partial apply, composition."""
    # Map with non-function
    yield "(require std/list all:)(display (map 42 '(1 2 3)))"
    # Map with wrong arity
    yield "(require std/list all:)(display (map (lambda (x y) (+ x y)) '(1 2 3)))"
    # Filter with non-predicate
    yield "(require std/list all:)(display (filter 42 '(1 2 3)))"
    # Foldl on empty with different init values
    yield "(require std/list all:)(display (foldl (lambda (x y) (+ x y)) 0 '()))"
    yield "(require std/list all:)(display (foldl (lambda (x y) (+ x y)) 'init '()))"
    # High-order type annotation
    yield "(display ((: (lambda (f x) (f x)) (-> (-> Int Int) Int Int)) (lambda (x) (+ x 1)) 5))"
    # Function composition
    yield ("(define (compose f g) (lambda (x) (f (g x))))"
           "(display ((compose (lambda (x) (* x 2)) (lambda (x) (+ x 1))) 5))")
    # Self-application (but bounded)
    yield ("(define (apply-twice f x) (f (f x)))"
           "(display (apply-twice (lambda (x) (+ x 1)) 5))")


# ═══════════════════════════════════════════════════════════
# 14. Macro — defmacro edge cases
# ═══════════════════════════════════════════════════════════
@register
def gen_macro_edge():
    """Macro definition edge cases: hygiene, nesting, expansion."""
    yield "(defmacro (when cond . body) (list 'if cond (cons 'begin body)))"
    yield "(display 1)"
    yield "(defmacro (unless cond . body) (list 'if (list 'not cond) (cons 'begin body)))"
    yield "(display 1)"
    # Macro that expands to macro call
    yield ("(defmacro (defn name args . body) (list 'define (cons name args) (cons 'begin body)))"
           "(defn (add a b) (+ a b))"
           "(display (add 1 2))")
    # Macro with unquote
    yield ("(defmacro (twice expr) (list '+ expr expr))"
           "(display (twice 5))")
    # Nested macros
    yield ("(defmacro (a x) (list 'display x))"
           "(defmacro (b y) (list 'a y))"
           "(b 42)")


# ═══════════════════════════════════════════════════════════
# 15. Conditionals — cond/if edge cases
# ═══════════════════════════════════════════════════════════
@register
def gen_cond_edge():
    """Conditional edge cases: cond with no clauses, if with void, nested."""
    yield "(cond)"
    yield "(cond (else 42))"
    yield "(cond (#f 1) (#f 2) (else 3))"
    yield "(if #t (display 1))"
    yield "(if #f (display 1))"
    yield "(if #t (display 1) (display 2))"
    yield "(if #f (display 1) (display 2))"
    # Cond as expression (return value)
    yield ("(display (cond ((> 3 2) 42) (else 0)))")
    # Nested if
    yield ("(display (if (> 5 3) (if (< 10 20) 'yes 'no) 'no))")
    # And/or with side effects
    yield ("(display (and #t #f))"
           "(display (or #f #t))")


# ═══════════════════════════════════════════════════════════
# 16. Binding — let/let*/letrec edge cases
# ═══════════════════════════════════════════════════════════
@register
def gen_binding_edge():
    """Binding construct edge cases: let shadowing, letrec cycles, named let."""
    # let with no bindings
    yield "(let () (display 42))"
    # let* with shadowing
    yield "(let* ((x 1) (x (+ x 1))) (display x))"
    # letrec with self-reference
    yield "(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (display (fact 5)))"
    # Named let (loop)
    yield ("(let loop ((i 5) (acc 1))"
           "  (if (= i 0) (display acc) (loop (- i 1) (* acc i))))")
    # Deep let binding
    yield ("(let ((a 1) (b 2) (c 3) (d 4) (e 5))"
           "  (display (+ a b c d e)))")
    # set! mutation
    yield ("(define x 1)(set! x 2)(display x)")# ═══════════════════════════════════════════════════════════
# 12. Arena / Memory — repeated define/eval, large AST
# ═══════════════════════════════════════════════════════════
@register
def gen_arena_stress():
    """Repeated compilation, arena allocation stress."""
    n = 20 if not QUICK else 5
    for _ in range(n):
        lines = []
        for i in range(100 if not QUICK else 20):
            op = rng.choice(["define", "display"])
            if op == "define":
                lines.append(f"(define (f{i} x) (+ x {rng.randint(0, 100)}))")
            lines.append(f"(display {rng.randint(0, 1000)})")
        yield "(begin " + " ".join(lines) + ")"


# ═══════════════════════════════════════════════════════════
# Runner
# ═══════════════════════════════════════════════════════════
DIMENSIONS = [
    ("stdlib-edge", gen_stdlib_edge, "Stdlib: edge case arguments"),
    ("type-system", gen_type_system, "Type: annotations, gradual, let-poly"),
    ("adt", gen_adt, "ADT: datatype definitions, match"),
    ("m4-linear", gen_m4_linear, "M4: borrow/move/drop sequences"),
    ("incremental", gen_incremental, "Incremental: serve redefine/dep tracking"),
    ("module", gen_module, "Module: require, load/unload"),
    ("edsl", gen_edsl, "EDSL: query/mutate/set-code chains"),
    ("hash-edge", gen_hash_edge, "Hash: missing keys, nested, large"),
    ("recursion-edge", gen_recursion_edge, "Recursion: TCO, mutual, Ackermann"),
    ("coercion", gen_coercion, "Coercion: type boundary, blame"),
    ("serve-structured", gen_serve_structured, "Serve: multi-session, protocol"),
    ("arena-stress", gen_arena_stress, "Arena: repeated compile, large AST"),
    ("higher-order", gen_higher_order, "HO: map/filter/compose edge cases"),
    ("macro-edge", gen_macro_edge, "Macro: defmacro edge cases"),
    ("cond-edge", gen_cond_edge, "Cond: conditional edge cases"),
    ("binding-edge", gen_binding_edge, "Binding: let/letrec/named-let edge cases"),
]


def run_code(code):
    ok, out, err = run(code)
    if ok:
        results["pass"] += 1
    else:
        results["fail"] += 1


def run_serve(code):
    """Send code lines through --serve stdin."""
    try:
        r = subprocess.run(
            [AURA, "--serve"],
            input=code + "\n",
            capture_output=True,
            text=True,
            timeout=TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        results["timeout"].append(code[:80])
        return
    if r.returncode < 0:
        sig = -r.returncode
        sig_name = {6: "SIGABRT", 8: "SIGFPE", 11: "SIGSEGV"}.get(sig, f"signal-{sig}")
        results["crash"].append((sig_name, f"[serve] {code[:60]}"))


def run_group(name, gen_fn):
    total = 0
    crashes_before = len(results["crash"])
    start = time.time()

    is_serve = name in ("incremental", "serve-structured")

    for code in gen_fn():
        total += 1
        if is_serve:
            run_serve(code)
        else:
            run_code(code)

    elapsed = time.time() - start
    new_crashes = len(results["crash"]) - crashes_before
    status = "💥 CRASH" if new_crashes else "✅"
    print(
        f"  {status} {name:25s} {total:3d} cases  "
        f"{elapsed:.1f}s  ({new_crashes} crashes)",
        flush=True,
    )


def main():
    print("=" * 60)
    print("Aura Structured Fuzz Suite")
    print(f"  Date:   {datetime.date.today().isoformat()}")
    print(f"  Seed:   {SEED if SEED is not None else 'random'}")
    print(f"  Binary: {AURA}")
    print(f"  Mode:   {'QUICK' if QUICK else 'FULL'}")
    print("=" * 60)

    total_start = time.time()

    for name, gen_fn, desc in DIMENSIONS:
        print(f"\n{'─'*60}")
        print(f"  {name}")
        print(f"  {desc}")
        print(f"{'─'*60}")
        run_group(name, gen_fn)

    elapsed = time.time() - total_start
    total = results["pass"] + results["fail"]
    print(f"\n{'='*60}")
    print(f"  Summary")
    print(f"{'='*60}")
    print(f"  Cases:    {total}")
    print(f"  Pass:     {results['pass']}")
    print(f"  Fail:     {results['fail']}")
    print(f"  Crashes:  {len(results['crash'])}")
    print(f"  Timeouts: {len(results['timeout'])}")
    print(f"  Elapsed:  {elapsed:.1f}s")

    if results["crash"]:
        print(f"\n  💥 CRASHES:")
        for sig, code in results["crash"][:10]:
            print(f"    {sig:15s} {code[:80]}")
        if len(results["crash"]) > 10:
            print(f"    ... and {len(results['crash']) - 10} more")

    if results["crash"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
