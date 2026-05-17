# Aura 测试框架设计

> 极简内置测试，零外部依赖。

---

## 现状

```
无测试框架。
只能手动 exec 看返回值。
```

---

## 设计

### 1. 核心断言

```scheme
; 内建原语（已存在）:
(assert expr)             ; 断言 expr 为真
(assert= expected actual) ; 断言相等
(error msg)               ; 抛异常（已存在，但崩进程）

; 新增:
(check expr)              ; 断言 expr 为真，失败时记录但不崩
(check= expected actual)  ; 断言相等，失败时记录但不崩
(check-error (lambda () (error "x")))  ; 断言抛异常
```

`check` 和 `check=` 在失败时不抛异常，而是返回一个结果值——这让测试可以继续运行。

### 2. Test Runner

```scheme
; (test-suite name body) — 定义测试套件
(test-suite "math"
  (check= 4 (square 2))
  (check= 9 (square 3))
  (check-error (lambda () (square "not-a-number"))))

; 内建运行函数:
(run-tests)  → 运行所有已定义的 test-suite

; 返回值:
'((suite "math"
    (check "square 2" passed)
    (check "square 3" passed)
    (check "square error" passed)))
```

### 3. 测试文件约定

```scheme
; tests/test-math.aura
(import "src/math.aura")

(test-suite "math:square"
  (check= 4 (square 2))
  (check= 0 (square 0))
  (check= 9 (square -3)))

(test-suite "math:sqrt"
  (check= 4 (sqrt 16))
  (check-error (lambda () (sqrt -1))))

; 文件末尾自动运行:
(run-tests)
```

### 4. CLI 集成

```bash
# 运行单个测试文件
./aura tests/test-math.aura

# 运行所有测试（todo）
./aura --test tests/

# 输出格式:
[PASS] math:square: square 2 = 4
[PASS] math:square: square 0 = 0
[PASS] math:square: square -3 = 9
[PASS] math:sqrt: sqrt 16 = 4
[PASS] math:sqrt: sqrt -1 throws
---------
5/5 passed
```

### 5. 实现

```
test-suite 是宏:
  (defmacro test-suite (name . body)
    `(begin
       (define __suite__ (list 'suite ',name))
       ,@(map (lambda (expr)
                `(let ((__result__ ,expr))
                   (set! __suite__ (cons (list 'check (quote ,expr) (if __result__ 'passed 'failed)) __suite__))))
              body)
       __suite__))

check 和 check= 是内建原语:
  (check expr)       → #t 或错误值（不抛异常）
  (check= a b)       → #t 或错误值

run-tests 是内建原语:
  搜索 env 中所有以 test-suite 定义收集的套件
  汇总结果
```

### 6. 覆盖率（远期）

```scheme
; 插桩:
(define (square x) (* x x))
; 启用覆盖率后等价于:
(define (square x)
  (begin
    (__cov-trace__ 'square 'entry)
    (let ((__r__ (* x x)))
      (__cov-trace__ 'square 'exit)
      __r__)))

; 查询:
(cov-report)  → 每个函数被调用的次数
```

### 7. 迁移路径

```
Phase 1: check / check= / check-error 原语  ← 现在
Phase 2: test-suite 宏
Phase 3: run-tests 原语
Phase 4: CLI --test 集成
Phase 5: 覆盖率（可选）
```
