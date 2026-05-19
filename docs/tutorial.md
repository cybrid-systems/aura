# Aura 语言入门

AI-native Lisp — C++26 + LLVM JIT + Sound Gradual Typing + C FFI。

```bash
cmake -B build && cmake --build build --target aura -j
echo '(+ 1 2 3)' | ./build/aura          # → 6 (JIT 自动加速)
./build/aura                              # REPL
```

---

## 一、基础

### 算术与比较
```scheme
(+ 1 2 3 4)          ;; → 10  多参数
(* 2 (+ 3 4))        ;; → 14  嵌套
(modulo 7 3)         ;; → 1
(= 5 5)              ;; → #t
(< 1 2 3)            ;; → #t  链式比较
```

### 布尔与字符串
```scheme
(and #t #f)          ;; → #f
(not (= 1 2))        ;; → #t
(string-append "hi" " " "there")  ;; → "hi there"
(string-length "hello")           ;; → 5
```

### 类型谓词
```scheme
(integer? 42)        ;; → #t
(float? 3.14)        ;; → #t
(string? "hi")       ;; → #t
(boolean? #t)        ;; → #t
(procedure? +)       ;; → #t
```

---

## 二、变量和函数

```scheme
(define x 42)
(define (square x) (* x x))
(square 5)                   ;; → 25

(define double (lambda (x) (* x 2)))
(double 10)                  ;; → 20

(let ((x 10) (y 20)) (+ x y))       ;; → 30
(let* ((x 2) (y (* x 3))) (+ x y)) ;; → 8

(letrec ((fact (lambda (n)
  (if (= n 0) 1 (* n (fact (- n 1)))))))
  (fact 5))                         ;; → 120

;; 命名 let（尾递归）
(let loop ((n 5) (acc 1))
  (if (= n 0) acc (loop (- n 1) (* acc n))))  ;; → 120
```

### 条件
```scheme
(if (< 2 3) "yes" "no")   ;; → "yes"
(when (> 5 3) (display "gt"))
(unless (= 1 2) (display "ok"))

(cond
  ((< 5 3) "less")
  ((= 5 3) "equal")
  (else "greater"))        ;; → "greater"
```

---

## 三、数据结构

### 列表
```scheme
(cons 1 (cons 2 (cons 3 '())))   ;; → (1 2 3)
(list 1 2 3)                     ;; → (1 2 3)
(car (list 1 2 3))               ;; → 1
(cdr (list 1 2 3))               ;; → (2 3)
(length (list 10 20 30))         ;; → 3
```

### 向量
```scheme
(vector 1 2 3)
(vector-ref (vector 10 20 30) 1)        ;; → 20
(vector-set! (vector 1 2 3) 0 99)       ;; 原地修改
(vector->list (vector 1 2 3))           ;; → (1 2 3)
```

### 哈希表
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

---

## 四、类型系统 — Sound Gradual

```scheme
;; 运行时类型反射
(type-of 42)            ;; → "Int"
(type-of "hi")          ;; → "String"
(type? 42 "Int")        ;; → #t

;; 类型标注
(: x Int)
(cast "42" : Int)       ;; → 42  运行时转换
(check 42 : Int)        ;; → 42  编译期验证

;; Occurrence typing — 分支自动细化
(if (string? x)
  (string-append x "!")     ;; 此处 x: String
  x)                         ;; 此处 x: not(String)

;; 严格模式
echo '(+ "hi" 1)' | ./build/aura --strict   ;; TypeError
```

---

## 五、标准库

```scheme
(require std/list all:)
(map (lambda (x) (* x 2)) (list 1 2 3))   ;; → (2 4 6)
(filter (lambda (x) (> x 2)) (list 1 2 3 4 5))  ;; → (3 4 5)
(foldl + 0 (list 1 2 3))                  ;; → 6
(range 1 6)                               ;; → (1 2 3 4 5)

(require std/json all:)
(json-parse "{\"a\":1}")                  ;; → hash

(require std/datetime)
(timestamp->iso-date (current-time))      ;; → "2026-05-19"

(require std/combinators)
(define f (compose (lambda (x) (+ x 1)) (lambda (x) (* x 2))))
(f 5)                                     ;; → 11
```

---

## 六、C FFI — 调用任意 C 库

```scheme
(define lib (c-load "libm.so.6"))

;; 数学函数
(define sqrt (c-func lib "sqrt" 2 2))   (sqrt 9.0)   ;; → 3.0
(define pow (c-func lib "pow" 2 2 2))   (pow 2.0 10) ;; → 1024.0

;; 字符串处理
(define strlen (c-func lib "strlen" 1 3))  (strlen "hello") ;; → 5
(define atoi (c-func lib "atoi" 1 3))       (atoi "42")      ;; → 42

;; 内存管理 (Opaque 指针)
(define malloc (c-func lib "malloc" 4 1))
(define free (c-func lib "free" 0 4))
(let ((buf (malloc 1024))) (free buf))
```

**类型标签:** 1=Int, 2=Float, 3=String, 4=Opaque, 0=Void

---

## 七、TCP 网络编程

```scheme
(begin
  (define fd (tcp-connect "httpbin.org" 80))
  (tcp-send fd "GET /get HTTP/1.0\r\nHost: httpbin.org\r\n\r\n")  ;; → 40 bytes
  (define resp (tcp-recv fd 4096))      ;; → "HTTP/1.1 200 OK..."
  (display resp)
  (tcp-close fd))

;; HTTP via curl helper (不需要解析 DNS)
(require std/socket)
(http-get "https://httpbin.org/get")             ;; → JSON 响应
(http-post "https://httpbin.org/post" "{\"a\":1}")  ;; → POST
```

**注意:** TCP 操作必须放在 `(begin ...)` 内（不要在 define 和 send 之间跨表达式）。

---

## 八、LLM 自生长 Agent

Aura 可以自己调用 LLM 生成、执行、修复代码：

```scheme
(define key (getenv "LLM_API_KEY"))
(define url "https://api.deepseek.com/chat/completions")

(define (llm-ask prompt)
  (http-post url 
    (string-append "{\"model\":\"deepseek-chat\",\"messages\":"
      "[{\"role\":\"user\",\"content\":\"" prompt "\"}]}") key))

;; 让 LLM 生成代码并执行
(define response (llm-ask "Write (reverse lst) in Aura"))
(define code (extract-code response))  ;; 解析 JSON → 提取代码
(define result (eval code))            ;; 执行生成代码
```

**自生长循环:** `llm-prompt → extract-code → try-eval → fix → repeat`

---

## 八、EDSL / AI Agent 开发

```lisp
;; AST 查询和变更是 Aura 的核心差异化能力

;; 1. 设置代码
(set-code "(define (bad-fact n) (* n (bad-fac (- n 1))))")

;; 2. 定位问题
(query:find "bad-fac")        ;; → (16)  节点 ID

;; 3. 自动修复
(mutate:rebind "bad-fac"
  "(define (bad-fact n) ...)"
  "fix typo")

;; 4. 获取修复后源码
(current-source)

;; 5. 验证
(eval-current)
(bad-fact 5)                  ;; → 120
```

---

## 九、错误处理

```scheme
(try
  (/ 1 0)
  (catch (e) (display "oops")))

(raise "something wrong")
(assert (= 1 1))
```

---

## 十、模块

```scheme
(require std/list)          ;; 带前缀: list:map, list:foldl
(import "std/list")          ;; 无前缀
(export map filter foldl)    ;; 声明公有接口
```

---

**下一步:** `docs/roadmap.md` · `docs/design/ffi_c.md` · `python3 tests/ai_agent_iter.py "task"`
