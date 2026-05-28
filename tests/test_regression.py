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
     "1", "match warning"),
    ("match-wildcard", '(define-type (C) (A) (B) (C)) (let ((x A)) (match x ((A) 1) ((_) 2)))',
     "1", ""),
    ("adt-exhaustive-option",
     '(define-type (Option a) (Some a) (None)) (let ((x (Some 42))) (match x ((Some v) v) ((None) 0)))',
     "42", ""),
    ("adt-exhaustive-color",
     '(define-type (Color) (Red) (Green) (Blue)) (let ((x Red)) (match x ((Red) 1) ((Green) 2) ((Blue) 3)))',
     "1", ""),
    ("adt-exhaustive-either",
     '(define-type (Either a b) (Left a) (Right b)) (let ((x (Left 1))) (match x ((Left v) v) ((Right v) 0)))',
     "1", ""),
    ("adt-exhaustive-nested",
     '(begin (define-type (Option a) (Some a) (None))(define-type (Result e v) (Ok v) (Err e))'
     '(let ((x (Ok (Some 42)))) (match x ((Ok y) (match y ((Some v) v) ((None) 0))) ((Err _) 0))))',
     "42", ""),
    ("adt-exhaustive-list",
     '(define-type (List a) (Nil) (Cons a (List a)))'
     '(let ((xs (Cons 1 (Cons 2 (Cons 3 Nil)))))'
     '(match xs ((Cons h t) (display (+ h 1))) ((Nil) 0)))',
     "2", ""),
    ("adt-missing-multi",
     '(define-type (Color) (Red) (Green) (Blue)) (let ((x Red)) (match x ((Red) 1) ((Green) 2)))',
     "1", "unhandled"),

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
    ("let-poly-id",
     '(let ((id (lambda (x) x))) (display (id 42))(display (id "hi"))(display (id #t)))', "42hi#t", ""),
    ("let-poly-nested",
     '(let ((f (lambda (x) x)))(let ((g (lambda (y) (f y))))(display (g 42))(display (g #t))))', "42#t", ""),
    ("let-poly-curry",
     '(let ((const (lambda (x) (lambda (y) x))))(display ((const 42) "ignored")))', "42", ""),
    ("let-poly-fst",
     '(let ((fst (lambda (p) (car p))))(display (+ (fst (cons 1 2)) (fst (cons 3 4)))))', "4", ""),
    ("let-poly-multi",
     '(let ((f (lambda (x) x))(g (lambda (x) x)))(display (f 1))(display (g 2)))', "12", ""),
    ("letrec-poly",
     '(letrec ((id (lambda (x) x)))(display (id 99))(display (id #f)))', "99#f", ""),

    # ── 增量类型检查 #4 ──────────────────────────────────
    ("incr-basic",
     '(begin (set-code "(define (add x y) (+ x y))")(display (typecheck-current))'
     '(typecheck-current))',
     "type: Void\nno errors", ""),
    ("incr-cache-hit",
     '(begin (set-code "(define (add x y) (+ x y))")(typecheck-current)(typecheck-current)(display "ok"))',
     "ok", ""),
    ("incr-mutate",
     '(begin (set-code "(define (add x y) (+ x y))")(typecheck-current)'
     '(mutate:rebind "add" "(lambda (a b) (* a b))" "test")(typecheck-current)(display "ok"))',
     "ok", ""),
    ("incr-multi-mutate",
     '(begin (set-code "(define (f x) (+ x 1))(define (g x) (f x))")(typecheck-current)'
     '(mutate:rebind "f" "(lambda (x) (* x 2))" "op")(typecheck-current)'
     '(mutate:rebind "g" "(lambda (x) (f x))" "chain")(typecheck-current)'
     '(display "ok"))',
     "ok", ""),
    ("incr-large-typecheck",
     '(begin (set-code "(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) (define (map f l) (if (null? l) (quote ()) (cons (f (car l)) (map f (cdr l)))))(define (sum l) (if (null? l) 0 (+ (car l) (sum (cdr l)))))")(typecheck-current)(typecheck-current)(typecheck-current)(display "ok"))',
     "ok", ""),
    ("incr-workspace-switch",
     '(begin (set-code "(define (f x) (+ x 1))")(typecheck-current)'
     '(define sandbox (workspace:create "sandbox"))(workspace:switch sandbox)'
     '(set-code "(define (f x) (* x 2))")(typecheck-current)(workspace:switch 0)'
     '(typecheck-current)(display "ok"))',
     "ok", ""),
    ("incr-caller-dirty",
     '(begin (set-code "(define (add x y) (+ x y))(define (caller z) (add z 1))(define (other z) (* z 2))")(typecheck-current)'
     '(mutate:rebind "add" "(lambda (a b) (* a b))" "mul")(typecheck-current)'
     '(display (eval-current))(display (caller 5))(display (other 5)))',
     "10", ""),
    ("incr-caller-chain",
     '(begin (set-code "(define (f x) (+ x 1))(define (g x) (f x))(define (h x) (g x))")(typecheck-current)'
     '(mutate:rebind "f" "(lambda (x) (* x 2))" "op")(typecheck-current)'
     '(display (eval-current))(display (h 5)))',
     "10", ""),
    # ── 依赖图驱动：set-body ────────────────────────────────
    ("dep-set-body",
     '(begin (set-code "(define (add x y) (+ x y))(define (calc x) (add x 1))")(typecheck-current)'
     '(mutate:set-body "add" "(* x y)")(typecheck-current)(eval-current)(display (calc 5)))',
     "5", ""),
    ("dep-set-body-chain",
     '(begin (set-code "(define (f x) (+ x 1))(define (g x) (f x))")(typecheck-current)'
     '(mutate:set-body "f" "(* x 2)")(typecheck-current)(eval-current)(display (g 5)))',
     "10", ""),
    # ── 依赖图驱动：多层调用链 ──────────────────────────────
    ("dep-chain-3",
     '(begin (set-code "(define (f x) (+ x 1))(define (g x) (f x))(define (h x) (g x))")(typecheck-current)'
     '(mutate:rebind "f" "(lambda (x) (* x 2))" "v2")(typecheck-current)'
     '(eval-current)(display (h 5)))',
     "10", ""),
    ("dep-chain-5",
     '(begin (set-code "(define (a x) (+ x 1))(define (b x) (a x))(define (c x) (b x))(define (d x) (c x))(define (e x) (d x))")(typecheck-current)'
     '(mutate:rebind "a" "(lambda (x) (* x 2))" "v2")(typecheck-current)'
     '(eval-current)(display (e 5)))',
     "10", ""),
    # ── 依赖图驱动：多次 mutate + interleaved typecheck ────
    ("dep-repeat-mutate",
     '(begin (set-code "(define (f x) (+ x 1))(define (g x) (f x))")(typecheck-current)'
     '(mutate:rebind "f" "(lambda (x) (* x 2))" "v1")(typecheck-current)'
     '(mutate:rebind "f" "(lambda (x) (- x 1))" "v2")(typecheck-current)'
     '(eval-current)(display (g 5)))',
     "4", ""),
    ("dep-tc-stress",
     '(begin (set-code "(define (f x) (+ x 1))(define (g x) (f x))")(typecheck-current)(typecheck-current)'
     '(mutate:rebind "f" "(lambda (x) (* x 2))" "v2")(typecheck-current)(typecheck-current)'
     '(typecheck-current)(eval-current)(display (g 5)))',
     "10", ""),

    # ── IR 类型信息 #5 ──────────────────────────────────────
    ("ir-annot-int", '(: x Int 42)', "42", ""),
    ("ir-annot-expr", '(: x Int (+ 1 2))', "3", ""),
    ("ir-annot-chain", '(+ (: x Int 1) (: y Int 2))', "3", ""),
    ("ir-annot-let", '(let ((x 10)) (: y Int x))', "10", ""),
    ("ir-annot-if", '(if 1 (: a Int 42) (: b Int 0))', "42", ""),

    # ── 模块类型签名 #8 ──────────────────────────────────────
    ("declare-type-basic",
     '(begin (declare-type "mytest" "Int Int" "Int")(display "ok"))',
     "ok", ""),
    ("declare-type-multi",
     '(begin (declare-type "f1" "Int" "Int")(declare-type "f2" "Int" "Bool")(display "ok"))',
     "ok", ""),

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

# ── .aura-type 签名自动加载 ───────────────────────────────
def test_aura_type_auto_load():
    """require should auto-load .aura-type files."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        mod_path = os.path.join(tmpdir, "mymod.aura")
        sig_path = os.path.join(tmpdir, "mymod.aura-type")
        with open(mod_path, "w") as f:
            f.write("(export add)\n(define (add x y) (+ x y))\n")
        with open(sig_path, "w") as f:
            f.write("add: Int Int -> Int\n")
        code = f'(begin (require "{mod_path}" all:)(set-code "(display (add 1 2))")(display (typecheck-current))(display "|")(eval-current))'
        r = subprocess.run([AURA], input=code, capture_output=True, text=True, timeout=10)
        assert "no errors" in r.stdout, f"type error with .aura-type: {r.stdout}"
        assert "3" in r.stdout, f"eval failed: {r.stdout}"
    print("  ✅ test-aura-type-auto-load")

def test_aura_type_no_sig():
    """Without .aura-type, typecheck should show unbound."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        mod_path = os.path.join(tmpdir, "mymod.aura")
        with open(mod_path, "w") as f:
            f.write("(export add)\n(define (add x y) (+ x y))\n")
        code = f'(begin (require "{mod_path}" all:)(set-code "(display (add 1 2))")(display (typecheck-current)))'
        r = subprocess.run([AURA], input=code, capture_output=True, text=True, timeout=10)
        assert "unbound variable" in r.stdout, f"expected unbound: {r.stdout}"
    print("  ✅ test-aura-type-no-sig")

# Run subprocess tests
passed_s = 0
failed_s = 0
for tf in [test_freeze_load, test_freeze_multi_expr, test_freeze_empty, test_emit_binary,
           test_aura_type_auto_load, test_aura_type_no_sig]:
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
