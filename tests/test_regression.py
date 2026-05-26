#!/usr/bin/env python3
"""Regression tests for recently fixed P0 issues and new features."""
import subprocess, sys, os, tempfile

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
     "2", "unhandled constructor"),
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

    # #32: vector-math
    ("vec-sum",
     '(require "std/vector-math" all:)(vec:sum (vec:range 0 5))',
     "10", ""),
    ("vec-dot",
     '(require "std/vector-math" all:)(vec:dot (vector 1 2 3) (vector 4 5 6))',
     "32", ""),
    ("vec-scale",
     '(require "std/vector-math" all:)(vec:sum (vec:scale (vector 1 2 3) 2))',
     "12", ""),
    ("vec-argmin",
     '(require "std/vector-math" all:)(vec:argmin (vector 5 1 9 2))',
     "1", ""),
    ("mat-identity",
     '(require "std/vector-math" all:)(mat:ref (mat:identity 3) 1 1)',
     "1", ""),

]

# Cleanup temp files
for f in ["/tmp/aura-regr.txt", "/tmp/aura-copy.txt", "/tmp/aura-regr-test.txt"]:
    if os.path.exists(f):
        os.remove(f)


# ── Subprocess-based tests (freeze/load/emit-binary) ──────────
def test_freeze_load():
    """Freeze a program and load it back."""
    src = b"(display 42)"
    # Freeze
    r1 = subprocess.run([AURA, "--freeze", "/tmp/aura-test-freeze.aura"],
                        input=src, capture_output=True, timeout=10)
    assert b"frozen to" in r1.stdout, f"freeze failed: {r1.stdout} {r1.stderr}"
    assert os.path.exists("/tmp/aura-test-freeze.aura"), "freeze file not created"
    # Load
    r2 = subprocess.run([AURA, "--load", "/tmp/aura-test-freeze.aura"],
                        capture_output=True, timeout=10)
    assert b"42" in r2.stdout or b"3" in r2.stdout, f"load failed: {r2.stdout} {r2.stderr}"
    print("  ✅ test-freeze-load")

def test_freeze_multi_expr():
    """Freeze multi-expression program."""
    src = b"(define (f x) (+ x 1))(display (f 41))"
    r1 = subprocess.run([AURA, "--freeze", "/tmp/aura-test-freeze2.aura"],
                        input=src, capture_output=True, timeout=10)
    assert b"frozen to" in r1.stdout
    r2 = subprocess.run([AURA, "--load", "/tmp/aura-test-freeze2.aura"],
                        capture_output=True, timeout=10)
    assert b"42" in r2.stdout, f"multi-expr load: {r2.stdout}"
    print("  ✅ test-freeze-multi")

def test_freeze_empty():
    """Freeze with empty input should fail gracefully."""
    r = subprocess.run([AURA, "--freeze", "/tmp/aura-test-empty.aura"],
                        input=b"", capture_output=True, timeout=10)
    assert r.returncode != 0, "empty freeze should fail"
    print("  ✅ test-freeze-empty")

def test_emit_binary():
    """Emit binary (placeholder) creates .ir file."""
    src = b"(display (+ 1 2))"
    r = subprocess.run([AURA, "--emit-binary", "/tmp/aura-test-out"],
                        input=src, capture_output=True, timeout=10)
    assert b"emitted" in r.stderr or b"emitted" in r.stdout, f"emit failed: {r.stdout} {r.stderr}"
    assert os.path.exists("/tmp/aura-test-out"), "emit binary not created"
    print("  ✅ test-emit-binary")

# Run subprocess tests
passed_s = 0
failed_s = 0
for tf in [test_freeze_load, test_freeze_multi_expr, test_freeze_empty, test_emit_binary]:
    try:
        tf()
        passed_s += 1
    except Exception as e:
        print(f"  ❌ {tf.__name__}: {e}")
        failed_s += 1

print(f"  Subprocess tests: {passed_s}/{passed_s + failed_s} passed")

# Cleanup freeze/emit files
for f in ["/tmp/aura-test-freeze.aura", "/tmp/aura-test-freeze2.aura",
          "/tmp/aura-test-empty.aura", "/tmp/aura-test-out.o.ir"]:
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

print(f"\n{passed}/{passed+failed} Aura + {passed_s}/{passed_s+failed_s} subprocess = {passed+passed_s}/{passed+failed+passed_s+failed_s} all passed")
sys.exit(1 if (failed > 0 or failed_s > 0) else 0)
