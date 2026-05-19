#!/usr/bin/env python3
"""Aura 生产测试 — 全栈验证

测试项：
1. EDSL 管线 (set-code → mutate → current-source → eval-current)
2. 编译 + 运行完整 Aura 标准库 (stdlib import)
3. 类型检查 + IR 双路径
4. 递归 + 闭包 + set! 可变状态
5. 错误处理 (try/catch/raise)
"""
import subprocess, sys, json, os, time

AURA = os.environ.get("AURA_BIN", "./build/aura")
PASS = 0
FAIL = 0

def test(name, code, expected, args=None):
    global PASS, FAIL
    cmd = [AURA] + (args or [])
    r = subprocess.run(cmd, input=code, capture_output=True, text=True, timeout=10)
    result = r.stdout.strip()
    stderr = r.stderr.strip()
    if result == expected:
        print(f"  ✅ {name}: {result}")
        PASS += 1
    else:
        print(f"  ❌ {name}: expected '{expected}', got '{result}'")
        if stderr:
            print(f"     stderr: {stderr[:100]}")
        FAIL += 1

def test_ir(name, code, expected):
    return test(name, code, expected, ["--ir"])

print("="*55)
print("Aura 生产测试 — 全栈验证")
print("="*55)

# ── 1. 基本特性 ──────────────────────────────────
print("\n── 1. Core语言特性 ──")
test("factorial (recursion)", """(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))(fact 10)""", "3628800")
test("fibonacci", """(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))(fib 20)""", "6765")
test("closure + set! (counter)", """(define (make-counter) (let ((count 0)) (lambda () (set! count (+ count 1)) count)))(define c (make-counter))(c)(c)(c)""", "3")
test("higher-order (map)", "(map (lambda (x) (* x 2)) (list 1 2 3))", "(2 4 6)")
test("variadic lambda", """((lambda (a b . rest) (list a b (length rest))) 1 2 3 4 5)""", "(1 2 3)")
test("letrec mutual", """(letrec ((even? (lambda (n) (if (= n 0) #t (if (< n 0) #f (odd? (- n 1))))))(odd? (lambda (n) (if (= n 0) #f (if (< n 0) #f (even? (- n 1)))))))(odd? 7)""", "#t")

# ── 2. 闭包桥接 + 类型系统 ──────────────────────────
print("\n── 2. 闭包 + 类型 ──")
test("closure bridge (map lambda)", "(map (lambda (x) (+ x 10)) (list 1 2 3))", "(11 12 13)")
test("filter closure", "(filter (lambda (x) (> x 2)) (list 1 2 3 4 5))", "(3 4 5)")
test("foldl closure", "(foldl (lambda (acc x) (+ acc x)) 0 (list 1 2 3 4 5))", "15")
test("typecheck (--strict)", """(define (add x y) (+ x y))(add 1 2)""", "3", ["--strict"])
test("hash-has-key?", """(hash-has-key? (hash "a" 1 "b" 2) "a")""", "#t")

# ── 3. 标准库 ──────────────────────────────────
print("\n── 3. 标准库 ──")
test("string-split", """(require std/string)(string-split "a b c")""", "(a b c)")
test("enum-from-to (range)", """(require std/list)(range 1 5)""", "(1 2 3 4 5)")
test("flatten", """(require std/list)(flatten (list (list 1 2) (list 3 4)))""", "(1 2 3 4)")
test("sort", """(require std/list)(sort (list 3 1 4 1 5 9) (lambda (a b) (< a b)))""", "(1 1 3 4 5 9)")
test("zip", """(require std/list)(zip (list 1 2 3) (list "a" "b" "c"))""", "((1 a) (2 b) (3 c))")

# ── 4. try/catch 错误处理 ──────────────────────────
print("\n── 4. 错误处理 (try/catch) ──")
test("try/catch success", "(try (+ 1 2) (catch (e) 0))", "3")
test("try/catch caught error", """(try (raise "oops") (catch (e) "caught"))""", "caught")
test("try/catch no raise", """(try "ok" (catch (e) "nope"))""", "ok")
test("error? predicate", """(error? (raise "err"))""", "#t")

# ── 5. IR 路径 ──────────────────────────────────
print("\n── 5. IR 路径 (--ir) ──")
test_ir("arithmetic", "(+ 1 2 3)", "6")
test_ir("factorial (self-ref)", "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))(fact 10)", "3628800")
test_ir("mutual recursion", "(define (even? n) (if (= n 0) #t (if (< n 0) #f (odd? (- n 1)))))(define (odd? n) (if (= n 0) #f (if (< n 0) #f (even? (- n 1)))))(odd? 7)", "#t")
test_ir("deep recursion (600)", "(define (deep n) (if (= n 0) 0 (deep (- n 1))))(deep 600)", "0")
test_ir("closure + set!", """(define (make-counter) (let ((count 0)) (lambda () (set! count (+ count 1)) count)))(define c (make-counter))(c)(c)(c)""", "3")
test_ir("try/catch", """(try (raise "err") (catch (e) "caught"))""", "caught")

# ── 6. EDSL 管线 ──────────────────────────────────
print("\n── 6. EDSL 管线 (--serve) ──")

proc = subprocess.Popen([AURA, "--serve"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
time.sleep(0.5)

# Send define
proc.stdin.write('{"cmd":"define","code":"(define (double x) (+ x x))","name":"double"}\n')
proc.stdin.flush()
time.sleep(0.3)
define_resp = proc.stdout.readline().strip()
if '"status":"defined"' in define_resp:
    print(f"  ✅ EDSL define: {define_resp[:60]}")
    PASS += 1
else:
    print(f"  ❌ EDSL define: {define_resp[:60]}")
    FAIL += 1

# Send exec
proc.stdin.write('{"cmd":"exec","code":"(double 21)"}\n')
proc.stdin.flush()
time.sleep(0.3)
exec_resp = proc.stdout.readline().strip()
if '"42"' in exec_resp:
    print(f"  ✅ EDSL exec: {exec_resp[:60]}")
    PASS += 1
else:
    print(f"  ❌ EDSL exec: {exec_resp[:60]}")
    FAIL += 1

proc.terminate()
proc.wait(timeout=5)
proc.wait(timeout=5)

# ── 7. Agent prompt (api-reference) ────────────────
print("\n── 7. Agent support ──")
test("api-reference returns primitives", "(api-reference)", "primitives", ["--ir"])
test("current-source returns code", """(set-code "(define (f x) x)")(current-source)""", "(define (f x) x)")

# ── Summary ──────────────────────────────────────
print(f"\n{'='*55}")
print(f"结果: {PASS}/{PASS+FAIL} 通过")
if FAIL > 0:
    print(f"⚠️  {FAIL} 个失败")
    sys.exit(1)
else:
    print("✅ 全部通过 — 全栈验证完成")
