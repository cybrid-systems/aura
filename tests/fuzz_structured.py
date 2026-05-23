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
    yield '(cdr \"hello\")'
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
    yield '(require std/hash all:)(define h (hash 1 \"a\" 2 \"b\"))(display (hash-keys h))'
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
    yield '(display ((: (lambda (x) (+ x 1)) (-> Int Int)) \"hello\"))'
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
    yield '(begin " + " ".join(lines) + ")'



# ═══════════════════════════════════════════════════════════
# 17. C FFI — c-func edge cases
# ═══════════════════════════════════════════════════════════
@register
def gen_ffi_edge():
    """FFI edge cases: wrong sig, missing lib, invalid types."""
    yield '(display (c-func -1 \"sqrt\" \'(Float) -> Float\' 9.0))'
    yield '(display (c-func -1 "strlen" "(String) -> Int" "hello"))'
    yield '(c-func -1 \"nonesuch\" \'(Int) -> Int\' 1)"  # symbol not foun'
    yield '(c-func 999 "sqrt" "(Float) -> Float" 9.0)'  # bad lib handle
    yield '(display (c-func -1 \"abs\" \'(Int) -> Int\' -5))'
    yield '(display (c-func -1 \"fabs\" \'(Float) -> Float\' -3.5))'
    yield '(display (c-func -1 \"toupper\" \'(Int) -> Int\' 97))'
    yield '(display (c-func -1 \"isdigit\" \'(Int) -> Int\' 48))'


# ═══════════════════════════════════════════════════════════
# 18. Serve Protocol — exec/mutate/redefine session management
# ═══════════════════════════════════════════════════════════
@register
def gen_serve_edge():
    """Serve protocol: exec with timeout, redefine, multi-line."""
    n = 20 if not QUICK else 5
    for _ in range(n):
        yield ('{"cmd":"session","name":"new:' + str(rng.randint(0,100)) + '"}')
        yield ('{"cmd":"exec","code":"(display ' + str(rng.randint(0,1000)) + ')"}')
        yield ('{"cmd":"exec","code":"(define (f x) (+ x ' + str(rng.randint(0,100)) + ')) (display (f 5))"}')
        yield ('{"cmd":"mutate","op":"record-patch","node":"0","op-name":"test","summary":"fuzz"}')
        yield ('{"cmd":"rollback","id":"1"}')
        yield ('{"cmd":"mutation-log","node":"0"}')
        yield ('{"cmd":"session","name":"session-' + str(rng.randint(0,10)) + '"}')


# ═══════════════════════════════════════════════════════════
# 19. Error Handling — try-catch, raise, error values
# ═══════════════════════════════════════════════════════════
@register
def gen_error_handling():
    """Try-catch edge cases: nested catch, raise void, catch all."""
    yield '(display (try (+ 1 2) (catch (e) 0)))'
    yield '(display (try (error "test") (catch (e) 42)))'
    yield '(display (try (display (+ 1 "hello")) (catch (e) "caught")))'
    yield '(display (try (error 42) (catch (e) e)))'
    yield '(display (try (try (error "inner") (catch (e) (error "outer"))) (catch (e) "caught")))'

def gen_string_edge():
    """String operations: edge cases with empty, special chars."""
    yield '(display (string-append "a" "b" "c"))'
    yield '(display (string-append "" ""))'
    yield '(display (string-length ""))'
    yield '(display (string-length "hello"))'
    yield '(display (string-ref "abc" 0))'
    yield '(display (string-ref "abc" 2))'
    yield '(display (substring "hello" 1 4))'
    yield '(display (string<? "a" "b"))'
    yield '(display (string=? "abc" "abc"))'
    yield '(display (number->string 42))'
    yield '(display (string->number "42"))'
    yield '(display (string->number "not-a-number"))'


# ═══════════════════════════════════════════════════════════
# 21. Number Edge Cases — overflow, division, sign
# ═══════════════════════════════════════════════════════════
@register
def gen_number_edge():
    """Number edge cases: division by zero, overflow, mixed sign."""
    yield "(display (/ 1 0))"
    yield "(display (/ 0 1))"
    yield "(display (quotient 10 3))"
    yield "(display (remainder 10 3))"
    yield "(display (quotient -10 3))"
    yield "(display (remainder -10 3))"
    yield "(display (* 2 3 4 5))"
    yield "(display (+ 1 2 3 4 5))"
    yield "(display (- 10))"
    yield "(display (- 10 20))"
    yield "(display (< 1 2 3))"
    yield "(display (< 3 2 1))"
    yield "(display (= 42 42))"
    yield "(display (= 42 43))"


# ═══════════════════════════════════════════════════════════
# 22. GC / Memory
# ═══════════════════════════════════════════════════════════
@register
def gen_gc_memory():
    """GC and memory introspection."""
    yield "(display (gc))"
    yield "(display (memory-stats))"
    yield "(begin (gc) (display 1))"
    yield "(begin (gc) (gc) (display 1))"
    yield "(begin (define x (range 0 1000)) (gc) (display (length x)))"


# ═══════════════════════════════════════════════════════════
# 23. Quote / Quasiquote edge cases
# ═══════════════════════════════════════════════════════════
@register
def gen_quote_edge():
    """Quote edge cases: nested quasiquote, unquote-splicing."""
    yield "'(1 2 3)"
    yield "`(1 2 3)"
    yield "`(1 ,(+ 1 2) 3)"
    yield "`(1 ,@'(2 3) 4)"
    yield "`(a `(b ,c) d)"
    yield "`(a ,(list 1 2 3) b)"
    yield "`(a ,@(list 1 2 3) b)"


# ═══════════════════════════════════════════════════════════
# 24. Thread / Concurrency (basic)
# ═══════════════════════════════════════════════════════════
@register

# ═══════════════════════════════════════════════════════════
# 25. Memory Stress — large alloc/free cycles, arena pressure
# ═══════════════════════════════════════════════════════════
@register
def gen_memory_stress():
    """Repeated large allocations and GC to stress arena allocator."""
    n = 20 if not QUICK else 5
    # Big list create/free cycles
    for _ in range(n):
        size = 1000 * (_ + 1)
        yield f"(require std/list all:)(define lst (range 0 {size}))(gc)(display (length lst))"
        yield f"(require std/list all:)(define lst (range 0 {size}))(gc)(gc)(display (length lst))"
    # Repeated define / GC
    for _ in range(n):
        yield ("(begin " +
               " ".join(f"(define x{i} {i})" for i in range(200)) +
               " (gc) (display 1))")
    # Alternating large/small
    yield ("(begin "
           "(define big (range 0 10000)) "
           "(define small 42) "
           "(gc) "
           "(define big2 (range 0 5000)) "
           "(gc) "
           "(display (+ (length big) (length big2))))")


# ═══════════════════════════════════════════════════════════
# 26. Closure Stress — large captured environments
# ═══════════════════════════════════════════════════════════
@register
def gen_closure_stress():
    """Closures capturing large environments, repeated calls."""
    n = 15 if not QUICK else 5
    for _ in range(n):
        # Closure capturing many variables
        bindings = " ".join(f"(x{i} {i})" for i in range(100))
        yield (f"(let ({bindings}) "
               f"(define (f) (+ x0 x50 x99)) "
               f"(display (f)))")
    # Closure in closure
    yield ("(define (make-adder n) "
           "(lambda (x) (+ x n))) "
           "(define add5 (make-adder 5)) "
           "(define add10 (make-adder 10)) "
           "(display (+ (add5 3) (add10 3)))")
    # Recursive closure
    yield ("(define Y (lambda (f) "
           "((lambda (x) (f (lambda (y) ((x x) y)))) "
           "(lambda (x) (f (lambda (y) ((x x) y))))))) "
           "(define fact (Y (lambda (f) (lambda (n) "
           "(if (= n 0) 1 (* n (f (- n 1)))))))) "
           "(display (fact 10)))")


# ═══════════════════════════════════════════════════════════
# 27. Hash Stress — many entries, repeated cycles
# ═══════════════════════════════════════════════════════════
@register
def gen_hash_stress():
    """Hash table with many entries and repeated set!/ref cycles."""
    n = 10 if not QUICK else 3
    for _ in range(n):
        entries = " ".join(f"{i} {i*2}" for i in range(500))
        yield (f"(require std/hash all:)"
               f"(define h (hash {entries}))"
               f"(display (hash-keys h))")
        # Set! many times
        yield ("(require std/hash all:)"
               "(define h (hash))" +
               "".join(f"(hash-set! h {i} {i*2})" for i in range(100)) +
               "(display (hash-ref h 50))")
    # Large hash, then GC, then access

# ═══════════════════════════════════════════════════════════
# 28. String Stress — large concat, substring cycles
# ═══════════════════════════════════════════════════════════
@register
def gen_string_stress():
    """String stress: repeated concatenation, large strings."""
    n = 10 if not QUICK else 3
    for _ in range(n):
        # Build a big string
        parts = " ".join(f'"x"' for _ in range(100))
        yield f"(display (string-append {parts}))"
    # Repeated substring
    yield '(display (substring "hello world this is a test string" 0 5))'
    yield '(display (substring "hello world this is a test string" 6 20))'
    # String comparison in loop
    yield ("(define (find strs target) "
           "(if (null? strs) -1 "
           "(if (string=? (car strs) target) 0 "
           "(+ 1 (find (cdr strs) target))))) "
           '(display (find \'(\"a\" \"b\" \"c\" \"d\" \"e\") "d")))')


# ═══════════════════════════════════════════════════════════
# 29. Deep Cycle — repeated set-code with growing AST
# ═══════════════════════════════════════════════════════════
@register
def gen_deep_cycle():
    """Repeated set-code with gradually larger programs — AST rebuild stress."""
    n = 10 if not QUICK else 3
    for size in [10, 50, 100, 200, 500]:
        if QUICK and size > 100:
            continue
        # set-code with many defines
        defines = " ".join(f"(define f{i} (lambda (x) (+ x {i})))" for i in range(size))
        yield f'(set-code "({defines} (display (f0 0)))")'
        yield f'(display (current-source))'
    # Rapid set-code cycles (same size, different content)
    for i in range(8 if not QUICK else 3):
        yield f'(set-code "(display {i})")'
        yield f'(eval-current)'


# ═══════════════════════════════════════════════════════════
# 30. Linear Stress — many borrow/move/drop sequences
# ═══════════════════════════════════════════════════════════
@register
def gen_linear_stress():
    """Linear ownership stress: many operations on linear values."""
    n = 15 if not QUICK else 5
    for _ in range(n):
        yield ("(define x 42) "
               "(linear:move x) "
               "(display 1)")  # x is moved, should error if used after
    for _ in range(n):
        yield ("(define x 42) "
               "(define y (linear:move x)) "
               "(display y)")
    # Borrow chain
    yield ("(define x 42) "
           "(linear:borrow x) "
           "(display x)")  # borrow is released, so display is ok
    # Long chain
    yield ("(define x 10) "
           "(define y (linear:move x)) "
           "(linear:borrow y) "
           "(linear:move y) "
           "(display 1)")


# ═══════════════════════════════════════════════════════════
# 31. Module Stress — repeated load/unload cycles
# ═══════════════════════════════════════════════════════════
@register
def gen_module_stress():
    """Module load/unload stress: require many modules repeatedly."""
    n = 20 if not QUICK else 5
    modules = ["std/list", "std/string", "std/pair", "std/hash", "std/vector-math"]
    for _ in range(n):
        mod = rng.choice(modules)
        yield f"(require {mod} all:)(display 1)"
        yield f'(use "{mod}")'
    # Require all at once
    yield "(require std/list all:)(require std/string all:)(require std/pair all:)(require std/hash all:)(display 1)"
    # Require then use again
    yield '(require std/list all:)(use "std/list")(display 1)"'
def gen_thread_edge():
    """Basic concurrency primitives if available."""
    yield "(display (thread? (current-thread)))"
    yield "(display (thread (lambda () 42)))"
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

    ("ffi-edge", gen_ffi_edge, "FFI: c-func edge cases, wrong signatures"),
    ("serve-edge", gen_serve_edge, "Serve: exec/redefine/mutate edge cases"),
    ("error-handling", gen_error_handling, "Error: try-catch, raise, error types"),
    ("string-edge", gen_string_edge, "String: indexing, compare, append edge cases"),
    ("number-edge", gen_number_edge, "Number: division by zero, overflow, sign"),
    ("gc-memory", gen_gc_memory, "Memory: (gc), (memory-stats), large allocations"),
    ("quote-edge", gen_quote_edge, "Quote: complex quasiquote, unquote-splicing"),

    ("memory-stress", gen_memory_stress, "Memory: large alloc/free cycles, arena pressure"),
    ("closure-stress", gen_closure_stress, "Closure: large env capture, repeated call"),
    ("hash-stress", gen_hash_stress, "Hash: many entries, repeated set! cycles"),
    ("string-stress", gen_string_stress, "String: large string concat, substring cycles"),
    ("deep-cycle", gen_deep_cycle, "Cycle: repeated set-code, deep AST rebuild"),
    ("linear-stress", gen_linear_stress, "Linear: many borrow/move/drop in sequence"),
    ("module-stress", gen_module_stress, "Module: repeated load/unload cycles"),
    ("thread-edge", gen_thread_edge, "Thread: basic threading if available"),
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
