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
    ("declare-type-void",
     '(begin (declare-type "logfn" "String" "Void")(display "ok"))',
     "ok", ""),
    ("declare-type-string",
     '(begin (declare-type "greet" "String" "String")(display "ok"))',
     "ok", ""),
    ("declare-type-bool",
     '(begin (declare-type "pred" "Int" "Bool")(display "ok"))',
     "ok", ""),
    ("declare-type-mixed",
     '(begin (declare-type "strlen" "String" "Int")(declare-type "double" "Int" "Int")(display "ok"))',
     "ok", ""),

    # ── Functor 泛型模块 ───────────────────────────────────────
    ("functor-define",
     '(begin (define-module (Stack :T) (export push)) (display "ok"))',
     "ok", ""),
    ("functor-instance",
     '(begin (define-module (M :T) (+ 1 2)) (M Int) (display "ok"))',
     "ok", ""),
    ("functor-multi-instance",
     '(begin (define-module (W :T) (display 1)) (W Int) (W String) (display "ok"))',
     "11ok", ""),
    ("functor-body",
     '(begin (define-module (Calc :T) (display (+ 40 2))) (Calc Int))',
     "42", ""),
    ("functor-multi-param",
     '(begin (define-module (Pair :A :B) (display 1)) (Pair Int String) (display "ok"))',
     "1ok", ""),
    # ── Functor 普通符号参数 ────────────────────────────────
    ("functor-plain-T",
     '(begin (define-module (Stack T) (display T)) (Stack Int))',
     "Int", ""),
    ("functor-plain-AB",
     '(begin (define-module (Pair A B) (display A)(display B)) (Pair Int String))',
     "IntString", ""),
    ("functor-plain-body",
     '(begin (define-module (Box T) (+ 1 2)) (Box Int) (display "ok"))',
     "ok", ""),
    ("functor-cache",
     '(begin (define-module (C T) (display 1)) (C Int) (C Int) (C Int) (display "ok"))',
     "1ok", ""),
    ("functor-export-tc",
     '(begin (define-module (S T) (export myfn) (define (myfn x) (* x 2))) (S Int) (display "ok"))',
     "ok", ""),
    ("functor-multi-cache",
     '(begin (define-module (M T) (display 1)) (M Int) (M String) (M Int) (M String) (display "ok"))',
     "11ok", ""),

    # ── DCE + 类型注解 ────────────────────────────────────────
    ("ir-annot-float", '(: x Float 3.14)', "3.14", ""),
    ("ir-annot-double", '(: x Int (+ (: a Int 1) (: b Int 2)))', "3", ""),

    # ── 复杂 ADT 穷尽性 ─────────────────────────────────────
    ("adt-exhaustive-pair",
     '(define-type (Pair a b) (MkPair a b))(let ((p (MkPair 1 2)))(match p ((MkPair x y) (+ x y))))',
     "3", ""),
    ("adt-exhaustive-triple",
     '(define-type (Triple a b c) (MkTriple a b c))(let ((t (MkTriple 1 2 3)))(match t ((MkTriple x y z) (+ x (+ y z)))))',
     "6", ""),

    # ── Capability Effects #9 ────────────────────────────────
    ("effect-type-IO", '(type-of 42)', "Int", ""),
    ("effect-mutate-basic",
     '(begin (set-code "(define (f x) (+ x 1))(display (f 41))")(typecheck-current)(eval-current))',
     "42", ""),

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

def test_aura_type_multi_func():
    """Multiple functions in .aura-type."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        mod_path = os.path.join(tmpdir, "lib.aura")
        sig_path = os.path.join(tmpdir, "lib.aura-type")
        with open(mod_path, "w") as f:
            f.write("(export add mul neg)\n")
            f.write("(define (add x y) (+ x y))\n")
            f.write("(define (mul x y) (* x y))\n")
            f.write("(define (neg x) (- 0 x))\n")
        with open(sig_path, "w") as f:
            f.write("add: Int Int -> Int\n")
            f.write("mul: Int Int -> Int\n")
            f.write("neg: Int -> Int\n")
        code = f'(begin (require "{mod_path}" all:)(set-code "(display (add 1 2))(display (mul 3 4))(display (neg 5))")(display (typecheck-current))(display "|")(eval-current))'
        r = subprocess.run([AURA], input=code, capture_output=True, text=True, timeout=10)
        assert "no errors" in r.stdout, f"type error: {r.stdout}"
        assert "3" in r.stdout and "12" in r.stdout and "-5" in r.stdout, f"eval wrong: {r.stdout}"
    print("  ✅ test-aura-type-multi-func")

def test_aura_type_different_types():
    """Different return types in .aura-type."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        mod_path = os.path.join(tmpdir, "fmt.aura")
        sig_path = os.path.join(tmpdir, "fmt.aura-type")
        with open(mod_path, "w") as f:
            f.write("(export greet)\n")
            f.write("(define (greet n) (string-append \"hi \" n))\n")
        with open(sig_path, "w") as f:
            f.write("greet: String -> String\n")
        # Use numeric arg to avoid string escaping issues in pipe mode
        code = f'(begin (require "{mod_path}" all:)(set-code "(display (greet 42))")(display (typecheck-current))(display "|")(eval-current))'
        r = subprocess.run([AURA], input=code, capture_output=True, text=True, timeout=10)
        assert "no errors" in r.stdout, f"type error: {r.stdout}"
        assert "hi 42" in r.stdout, f"eval wrong: {r.stdout}"
    print("  ✅ test-aura-type-different-types")

def test_aura_type_cross_module():
    """Cross-module type checking with 2 modules."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        # Module A: math operations
        a_path = os.path.join(tmpdir, "math.aura")
        a_sig = os.path.join(tmpdir, "math.aura-type")
        with open(a_path, "w") as f:
            f.write("(export square)\n(define (square x) (* x x))\n")
        with open(a_sig, "w") as f:
            f.write("square: Int -> Int\n")
        # Module B: uses math
        b_path = os.path.join(tmpdir, "calc.aura")
        b_sig = os.path.join(tmpdir, "calc.aura-type")
        with open(b_path, "w") as f:
            f.write(f'(require "{a_path}" all:)(export sum-sq)\n')
            f.write('(define (sum-sq x y) (+ (square x) (square y)))\n')
        with open(b_sig, "w") as f:
            f.write("sum-sq: Int Int -> Int\n")
        # Main: uses calc
        code = f'(begin (require "{b_path}" all:)(set-code "(display (sum-sq 3 4))")(display (typecheck-current))(display "|")(eval-current))'
        r = subprocess.run([AURA], input=code, capture_output=True, text=True, timeout=10)
        assert "no errors" in r.stdout, f"cross-module type error: {r.stdout}"
        assert "25" in r.stdout, f"eval wrong (expected 25): {r.stdout}"
    print("  ✅ test-aura-type-cross-module")

def test_generate_type_sigs():
    """generate-type-sigs creates .aura-type from module."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        mod_path = os.path.join(tmpdir, "gen.aura")
        sig_path = os.path.join(tmpdir, "gen.aura-type")
        with open(mod_path, "w") as f:
            f.write("(define (myfn x) (* x 2))\n")
        code = f'(generate-type-sigs "{mod_path}")(require "{mod_path}" all:)(display (myfn 42))'
        r = subprocess.run([AURA], input=code, capture_output=True, text=True, timeout=10)
        assert "84" in r.stdout, f"generate+require+eval wrong: {r.stdout}"
        assert os.path.exists(sig_path), ".aura-type not created"
        with open(sig_path) as f:
            content = f.read()
            assert "myfn" in content, f"sig missing myfn: {content}"
    print("  ✅ test-generate-type-sigs")

def test_module_chain_5():
    """5-module chain: A→B→C→D→E with generate-type-sigs."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        # Module chain: E calls D calls C calls B calls A
        # Each module uses generate-type-sigs for the NEXT module
        mods = {}
        for name in ["a", "b", "c", "d", "e"]:
            mods[name] = os.path.join(tmpdir, f"{name}.aura")
        
        # Write modules (reverse order so depends on already-written ones)
        # a: base
        with open(mods["a"], "w") as f:
            f.write("(define (fn-a x) (* x 2))\n")
        # b: calls a
        with open(mods["b"], "w") as f:
            f.write(f'(require "{mods["a"]}" all:)\n')
            f.write("(define (fn-b x) (fn-a (+ x 1)))\n")
        # c: calls b
        with open(mods["c"], "w") as f:
            f.write(f'(require "{mods["b"]}" all:)\n')
            f.write("(define (fn-c x) (fn-b (* x 3)))\n")
        # d: calls c
        with open(mods["d"], "w") as f:
            f.write(f'(require "{mods["c"]}" all:)\n')
            f.write("(define (fn-d x) (fn-c (+ x 10)))\n")
        # e: calls d (entry point)
        with open(mods["e"], "w") as f:
            f.write(f'(require "{mods["d"]}" all:)\n')
            f.write("(define (fn-e x) (fn-d (- x 5)))\n")
        
        # Generate type sigs for all modules
        steps = f'(generate-type-sigs "{mods["a"]}")'
        for name in ["b", "c", "d", "e"]:
            steps += f'(generate-type-sigs "{mods[name]}")'
        
        # Test: require e, call fn-e
        code = f'(begin {steps}(require "{mods["e"]}" all:)(display (fn-e 20)))'
        r = subprocess.run([AURA], input=code, capture_output=True, text=True, timeout=10)
        # fn-e(20) = fn-d(20-5) = fn-c(15+10) = fn-b(25*3) = fn-a(75+1) = 76*2 = 152
        assert "152" in r.stdout, f"chain eval wrong: {r.stdout}"
        
        # Verify all sig files created
        for name in ["a", "b", "c", "d", "e"]:
            sig = os.path.join(tmpdir, f"{name}.aura-type")
            assert os.path.exists(sig), f"{name}.aura-type not created"
    print("  ✅ test-module-chain-5")

def test_abf_embed_sig():
    """ABF embed: .aura-type embedded in .abfc cache."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        mod_path = os.path.join(tmpdir, "m.aura")
        sig_path = os.path.join(tmpdir, "m.aura-type")
        with open(mod_path, "w") as f:
            f.write("(define (f x) (* x 2))\n")
        with open(sig_path, "w") as f:
            f.write("f: Int -> Int\n")
        # First generate sigs to create .aura-type
        r1 = subprocess.run([AURA], input=f'(generate-type-sigs "{mod_path}")',
                           capture_output=True, text=True, timeout=10, cwd=tmpdir)
        # Use the compiler to call check-module-signature (reads .aura-type)
        r2 = subprocess.run([AURA], input=f'(generate-type-sigs "{mod_path}")(require "{mod_path}" all:)(display (f 21)))',
                           capture_output=True, text=True, timeout=10, cwd=tmpdir)
        assert "42" in r2.stdout, f"embed+eval failed: {r2.stdout}"
    print("  ✅ test-abf-embed-sig")

# Run subprocess tests
passed_s = 0
failed_s = 0
for tf in [test_freeze_load, test_freeze_multi_expr, test_freeze_empty, test_emit_binary,
           test_aura_type_auto_load, test_aura_type_no_sig,
           test_aura_type_multi_func, test_aura_type_different_types,
           test_aura_type_cross_module,
           test_generate_type_sigs,
           test_module_chain_5]:
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
