#!/usr/bin/env python3
"""Regression tests for recently fixed P0 issues and new features."""
import subprocess, sys

AURA = "./build/aura"

def run(code, timeout=10):
    r = subprocess.run([AURA], input=code, capture_output=True, text=True, timeout=timeout)
    return r.stdout.strip(), r.stderr.strip(), r.returncode

tests = [
    # ── P0: 编译器缺陷 ────────────────────────────────────
    ("annot-3arg-int", '(: x Int 42)', "42", ""),
    ("annot-3arg-expr", '(: x Int (+ 1 2))', "3", ""),
    ("annot-chain", '(+ (: x Int 1) 2)', "3", ""),
    ("annot-unbound", '(: x Int)', "", "unbound variable"),
    ("annot-in-expr", '(+ (: total Int 10) 5)', "15", ""),
    ("blame-add-str", '(+ 1 "hello")', "1", "type mismatch"),

    # ── M4 线性所有权 ─────────────────────────────────────
    ("linear-eval", '(set-code "(display (move 42))") (eval-current)', "42", ""),
    ("m4-move-let", '(let ((x (move 42))) (display x))', "42", ""),
    ("m4-linear-move", '(display (move (Linear 42)))', "42", ""),
    ("m4-borrow", '(display (borrow 42))', "42", ""),
    ("m4-drop", '(display (drop 42))', "", ""),

    # ── match 穷尽性 ──────────────────────────────────────
    ("match-missing", '(define-type (C) (A) (B) (C)) (let ((x A)) (match x ((A) 1) ((B) 2)))',
     "2", "missing constructor"),
    ("match-wildcard", '(define-type (C) (A) (B) (C)) (let ((x A)) (match x ((A) 1) ((_) 2)))',
     "2", ""),

    # ── Phase 2: eval-current-output ──────────────────────
    ("eval-capture",
     '(begin (set-code "(display 42)") (eval-current-output))', "42", ""),

    # ── colony:search ─────────────────────────────────────
    ("colony-search",
     '(begin (set-code "(define (f x) (+ x 1))(display (f 41))")(require "std/ant" all:)(colony:search "42" 5))',
     "colony:ref-42", ""),
    ("colony-no-fns",
     '(begin (set-code "(display 42)")(require "std/ant" all:)(colony:search "42" 5))',
     "colony:no-fns", ""),

    # ── mutate:tweak-literal ──────────────────────────────
    ("tweak-lit-plus",
     '(begin (set-code "(display (+ 1 42))")(eval-current)(mutate:tweak-literal 3 1 "")(eval-current))',
     "44", ""),

    # ── P3: measure-distance / pid ─────────────────────────
    ("pid-measure",
     '(require "std/adaptive" all:)(measure-distance 0 "30" \'("42"))', "fine", ""),

    # ── 类型系统 ──────────────────────────────────────────
    ("lambda-annot-param",
     '((lambda ((: x Int)) (+ x 1)) 41)', "42", ""),
    ("lambda-annot-param2",
     '((lambda ((: x Int)) x) 99)', "99", ""),
    ("occ-integer",
     '(let ((x 42)) (if (integer? x) (+ x 1) 0))', "43", ""),
    ("occ-hash",
     '(let ((x (hash))) (if (hash? x) 42 0))', "42", ""),
    ("let-poly", '((lambda (x) (list (x 1) (x "hi"))) (lambda (y) y))', '(1 "hi")', ""),

    # ── closure warning → stdout ──────────────────────────
    ("closure-warning-stdout",
     '(define (f x) (+ x 1)) f', "uncalled function", ""),

    # ── FFI 诊断 ──────────────────────────────────────────
    ("ffi-error-stdout",
     '(c-func 999 "nonexistent" "(String) -> Int")', "invalid library", ""),
    ("ffi-sig-missing-arrow",
     '(c-func -1 "strlen" "invalid")', "missing '->'", ""),
    ("ffi-sig-unknown-type",
     '(c-func -1 "strlen" "(String) -> Unknown")', "unknown type: Unknown", ""),
    ("ffi-sig-valid-types-list",
     '(c-func -1 "strlen" "(String) -> Unknown")', "Int, Float, String", ""),
    ("ffi-symbol-not-found",
     '(c-func -1 "nonexistent_fn_xyz" "(String) -> Int")', "not found in library", ""),

    # ── File I/O ──────────────────────────────────────────
    ("file-write-read",
     '(begin (write-file "/tmp/aura-regr.txt" "hello 42")(read-file "/tmp/aura-regr.txt"))',
     "hello 42", ""),
    ("file-exists",
     '(file-exists? "/tmp/aura-regr.txt")', "1", ""),
    ("file-exists-not",
     '(file-exists? "/tmp/aura-nonexistent-xyz")', "0", ""),
    ("file-size",
     '(file-size "/tmp/aura-regr.txt")', "8", ""),
    ("file-delete",
     '(begin (write-file "/tmp/aura-del-test.txt" "x")(file-delete "/tmp/aura-del-test.txt")(file-exists? "/tmp/aura-del-test.txt"))',
     "0", ""),
    ("file-copy",
     '(begin (file-copy "/tmp/aura-regr.txt" "/tmp/aura-copy.txt")(read-file "/tmp/aura-copy.txt"))',
     "hello 42", ""),
    ("directory-list",
     '(directory-list "/tmp")', "aura-regr.txt", ""),

    # ── try-catch ─────────────────────────────────────────
    ("try-catch-ok",
     '(try "ok" (catch (e) "error"))', "ok", ""),
    ("try-catch-error",
     '(try (display (car ())) (catch (e) "caught"))', "caught", ""),

    # ── shell / command-output ────────────────────────────
    ("command-output",
     '(command-output "echo 42")', "42", ""),
    ("command-output-multi",
     '(command-output "echo hello; echo world")', "hello\nworld", ""),
    ("command-output-empty",
     '(command-output "echo -n")', "", ""),

    # ── command-line ──────────────────────────────────────
    ("cmdline-empty",
     '(command-line)', "", ""),

]

# Cleanup temp files
import os
for f in ["/tmp/aura-regr.txt", "/tmp/aura-copy.txt", "/tmp/aura-regr-test.txt"]:
    if os.path.exists(f):
        os.remove(f)

passed = 0
failed = 0
for name, code, expect_out, expect_err in tests:
    out, err, rc = run(code)
    ok = True
    if expect_out:
        if expect_out.startswith("(") and expect_out.endswith(")"):
            # Parenthesized output: exact match
            if out != expect_out:
                ok = False
        else:
            if expect_out not in out:
                ok = False
    if expect_err:
        # Support regex-like patterns
        import re
        if not re.search(expect_err, err):
            ok = False
    if ok:
        print(f"  ✅ {name}")
        passed += 1
    else:
        print(f"  ❌ {name}: expected out~{expect_out!r} err~{expect_err!r}, got out={out!r} err={err!r}")
        failed += 1

print(f"\n{passed}/{passed+failed} passed")
sys.exit(1 if failed else 0)
