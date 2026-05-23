#!/usr/bin/env python3
"""Regression tests for recently fixed P0 issues and new features."""
import subprocess, sys

AURA = "./build/aura"

def run(code):
    r = subprocess.run([AURA], input=code, capture_output=True, text=True, timeout=10)
    return r.stdout.strip(), r.stderr.strip(), r.returncode

tests = [
    # P0.3: (: name Type val) 3-arg form
    ("annot-3arg-int", '(: x Int 42)', "42", ""),
    ("annot-3arg-expr", '(: x Int (+ 1 2))', "3", ""),
    ("annot-chain", '(+ (: x Int 1) 2)', "3", ""),
    
    # P0.2: (: x Int) with unbound x should error
    ("annot-unbound", '(: x Int)', "", "unbound variable"),
    
    # P0.4: (+ 1 "hello") should emit blame to stderr  
    ("blame-add-str", '(+ 1 "hello")', "1", "type mismatch"),
    
    # P0.1: M4 nodes in tree-walker should not crash (test via EDSL)
    ("linear-eval", '(set-code "(display (move 42))") (eval-current)', "42", ""),
    
    # 3-arg annotation works in expression context
    ("annot-in-expr", '(+ (: total Int 10) 5)', "15", ""),
    
    # Match exhaustiveness: missing constructor
    ("match-missing", '(define-type (C) (A) (B) (C)) (let ((x A)) (match x ((A) 1) ((B) 2)))',
     "2", "missing constructor"),
    
    # Match with wildcard (no error expected)
    ("match-wildcard", '(define-type (C) (A) (B) (C)) (let ((x A)) (match x ((A) 1) ((_) 2)))',
     "2", ""),
    
    # M4: move in let works (was: parse bug)
    ("m4-move-let", '(let ((x (move 42))) (display x))', "42", ""),
    
    # M4: linear + move
    ("m4-linear-move", '(display (move (Linear 42)))', "42", ""),
    
    # M4: borrow in expression (value, no variable binding)
    ("m4-borrow", '(display (borrow 42))', "42", ""),
    
    # M4: &x reader macro (needs bound variable)
    ("m4-drop", '(display (drop 42))', "", ""),

    # Phase 2: eval-current-output captures display output
    ("eval-capture",
     '(begin (set-code "(display 42)") (eval-current-output))',
     "42", ""),

    # Phase 2: colony:search finds function and mutates it
    ("colony-search",
     '(begin (set-code "(define (f x) (+ x 1))(display (f 41))")(require "std/ant" all:)(colony:search "42" 5))',
     "colony:ref-42", ""),

    # colony:search with no functions (no-op)
    ("colony-no-fns",
     '(begin (set-code "(display 42)")(require "std/ant" all:)(colony:search "42" 5))',
     "colony:no-fns", ""),

    # #18: mutate:tweak-literal on LiteralInt (verify via eval output)
    ("tweak-lit-plus",
     '(begin (set-code "(display (+ 1 42))")(eval-current)(mutate:tweak-literal 3 1 "")(eval-current))',
     "44", ""),

    # P3: measure-distance (not pid:analyze — Aura-only helper)
    ("pid-measure",
     '(require "std/adaptive" all:)(measure-distance 0 "30" \'("42"))',
     "fine", ""),

    # Type-annot lambda params ((: x Int))
    ("lambda-annot-param",
     '((lambda ((: x Int)) (+ x 1)) 41)',
     "42", ""),

    # closure warning on stdout (was stderr)
    ("closure-warning-stdout",
     '(define (f x) (+ x 1)) f',
     "uncalled function", ""),

    # ffi: c-func error goes to stdout (was stderr)
    ("ffi-error-stdout",
     '(c-func 999 "nonexistent" "(String) -> Int")',
     "invalid library", ""),


    # Type-annot lambda params ((: x Int))
    ("lambda-annot-param",
     '((lambda ((: x Int)) (+ x 1)) 41)',
     "42", ""),

    # Occurrence: integer? predicate (newly added)
    ("occ-integer",
     '(let ((x 42)) (if (integer? x) (+ x 1) 0))',
     "43", ""),

    # Occurrence: hash? predicate (newly added)
    ("occ-hash",
     '(let ((x (hash))) (if (hash? x) 42 0))',
     "42", ""),

]

passed = 0
failed = 0
for name, code, expect_out, expect_err in tests:
    out, err, rc = run(code)
    ok = True
    if expect_out and expect_out not in out:
        ok = False
    if expect_err and expect_err not in err:
        ok = False
    if ok:
        print(f"  ✅ {name}")
        passed += 1
    else:
        print(f"  ❌ {name}: expected out~{expect_out!r} err~{expect_err!r}, got out={out!r} err={err!r}")
        failed += 1

print(f"\n{passed}/{passed+failed} passed")
sys.exit(1 if failed else 0)
