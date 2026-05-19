# Aura 语言入门

Aura 是一个 AI-native Lisp：C++26 实现、LLVM JIT 原生编译、Sound Gradual Typing。

## 运行

```bash
echo '(+ 1 2 3)' | ./build/aura          # → 6 (JIT 自动加速)
./build/aura                              # REPL
```

## 基础

```scheme
;; 算术 (多参数, 自动 JIT)
(+ 1 2 3 4)           ;; → 10
(* 2 (+ 3 4))         ;; → 14
(modulo 7 3)          ;; → 1

;; 比较 (返回 #t / #f)
(= 5 5)               ;; → #t
(< 1 2 3)             ;; → #t  链式

;; 布尔
(and #t #f)           ;; → #f
(not (= 1 2))         ;; → #t

;; 字符串
(string-append "hi" " " "there")  ;; → "hi there"
(string-length "hello")           ;; → 5
```

## 变量和函数

```scheme
(define x 42)
(define (square x) (* x x))
(square 5)                        ;; → 25

;; lambda
(define double (lambda (x) (* x 2)))
(double 10)                       ;; → 20

;; let / let* / letrec
(let ((x 10) (y 20)) (+ x y))     ;; → 30
(let* ((x 2) (y (* x 3))) (+ x y))  ;; → 8
(letrec ((fact (lambda (n)
  (if (= n 0) 1 (* n (fact (- n 1)))))))
  (fact 5))                       ;; → 120

;; 命名 let (尾递归)
(let loop ((n 5) (acc 1))
  (if (= n 0) acc (loop (- n 1) (* acc n))))  ;; → 120
```

## 条件

```scheme
(if (< 2 3) "yes" "no")       ;; → "yes"
(when (> 5 3) (display "gt")) ;; → "gt"
(unless (= 1 2) (display "ok"))

(cond
  ((< 5 3) "less")
  ((= 5 3) "equal")
  (else "greater"))           ;; → "greater"
```

## 列表

```scheme
(cons 1 (cons 2 (cons 3 '())))   ;; → (1 2 3)
(list 1 2 3)                     ;; → (1 2 3)
(car (list 1 2 3))               ;; → 1
(cdr (list 1 2 3))               ;; → (2 3)
(length (list 10 20 30))         ;; → 3

;; 标准库
(require std/list all:)
(map (lambda (x) (* x 2)) (list 1 2 3))   ;; → (2 4 6)
(filter (lambda (x) (> x 2)) (list 1 2 3 4 5))  ;; → (3 4 5)
(foldl + 0 (list 1 2 3))                  ;; → 6
(range 1 6)                               ;; → (1 2 3 4 5)
```

## 哈希表

```scheme
(define h (hash "a" 1 "b" 2))
(hash-ref h "a")                 ;; → 1
(hash-set! h "c" 3)
(hash-length h)                  ;; → 3
(hash-keys h)                    ;; → ("a" "b" "c")

;; 函数式
(require std/hash)
(hash->list (hash "x" 10))              ;; → (("x" 10))
(hash-merge (hash "a" 1) (hash "b" 2))
```

## 类型系统 — Sound Gradual

```scheme
;; 运行时类型反射
(type-of 42)            ;; → "Int"
(type-of "hi")          ;; → "String"
(type? 42 "Int")        ;; → #t

;; 标注语法
(: x Int)
(cast "42" : Int)       ;; → 42  运行时转换
(check 42 : Int)        ;; → 42  编译期验证

;; occurrence typing — 分支自动细化
(if (string? x)
  (string-append x "!")   ;; 此处 x: String
  x)                       ;; 此处 x: not(String)

;; --strict 模式
echo '(+ "hi" 1)' | ./build/aura --strict   ;; TypeError
```

## 错误处理

```scheme
(try
  (/ 1 0)
  (catch (e) (display "oops: ") (display e)))

(raise "something wrong")
(assert (= 1 1))
```

## 标准库

```scheme
;; 日期时间
(require std/datetime)
(timestamp->iso-date (current-time))  ;; → "2026-05-19"

;; JSON
(require std/json)
(json-parse "{\"a\":1}")              ;; → (("a" 1))

;; CSV
(require std/csv)
(csv-parse "a,b\n1,2")                ;; → (("a" "b") ("1" "2"))

;; 组合器
(require std/combinators)
(define f (compose (lambda (x) (+ x 1)) (lambda (x) (* x 2))))
(f 5)                                 ;; → 11
```

## 模块

```scheme
(require std/list)          ;; 带前缀: list:map, list:foldl
(import "std/list")          ;; 无前缀
(export map filter foldl)    ;; 声明公有接口
```

## 宏

```scheme
(defmacro (twice x) (+ x x))
(twice 5)                     ;; → 10

(defmacro (my-if cond then else)
  `(if ,cond ,then ,else))    ;; 准引用模板
```

## 下一步

- [docs/roadmap.md](./roadmap.md) — 路线图
- [stdlib source](https://github.com/cybrid-systems/aura/tree/main/lib/std) — 标准库源码
- `./build/aura --serve` — CaaS JSON 服务
- `python3 tests/ai_agent_iter.py "Task"` — LLM agent 开发
