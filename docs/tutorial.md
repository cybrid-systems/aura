# Aura 语言入门

**语言为 AI 而生。**

Aura 不是给人类写的语言——是给 AI 写的。
但你仍然可以用它，就像你会骑 AI 设计的自行车。

```bash
cmake -B build && cmake --build build --target aura -j
echo '(+ 1 2 3)' | ./build/aura          # → 6
echo '(display 42)' | ./build/aura  # → 42
echo '(display (list 1 2 3))' | ./build/aura  # → (1 2 3)
```

---

### 这一设计的背后

Aura 的语法像 Scheme，但它的灵魂不是。
大多数语言为可读性优化——Aura 为 AI 的可操作性优化。

具体来说：
- **EDSL**: 代码不是写出来的，是自己长出来的（`set-code` → `mutate` → `eval-current`）
- **控制论修复**: 不是一次写对，而是测量距离、局部搜索、逐步逼近
- **Scheme 兼容**: LLM 写 `(first x)` 或 `(char=? c #\()` 自然通过

先学会常规用法，再深入 AI 驱动开发。

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
(symbol? 'x)         ;; → #t
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

;; 频次统计
(require std/hash all:)
(frequencies (list 1 2 1 3 1 2))         ;; → hash{1:3, 2:2, 3:1}
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

;; 一致性检查
(consistent-subtype Int Int)     ;; → #t
(consistent-subtype Int Dynamic) ;; → #t

;; ADT 类型
(define-type (Option a) (Some a) (None))
(type-of (Some 42))               ;; → (Option Int)
(type-of None)                    ;; → (Option a)
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
(json-stringify (hash "a" 1 "b" 2))       ;; → {"a":1,"b":2}

;; 纯内置 JSON 原语（无需 require）
(json-encode (list 1 "two" #t))            ;; → [1,"two",true]
(json-parse "{\"a\":[1,2,3]}")            ;; → hash key:"a" val:(1 2 3)

(require std/datetime)
(timestamp->iso-date (current-time))      ;; → "2026-05-19"

(require std/combinators)
(define f (compose (lambda (x) (+ x 1)) (lambda (x) (* x 2))))
(f 5)                                     ;; → 11

;; 数学模块 — 40+ 函数, 6 个类别
(require std/math all:)
(pi)                                      ;; → 3.14159...
(sin 0.0)                                 ;; → 0.0
(floor 3.7)                               ;; → 3.0
(log 1.0)                                 ;; → 0.0
(random-integer 1 100)                    ;; → 随机整数
(factorial 5)                             ;; → 120

;; 数据模块
(require std/data all:)
(pair? (cons 1 2))                        ;; → #t
(list->string '(65 66 67))                ;; → "ABC"

;; 算法模块
(require std/algorithm all:)
(search (list 1 3 5 7) 5)                 ;; → 2
(sort (list 3 1 4 1 5) <)                 ;; → (1 1 3 4 5)
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

Aura 内置 `(intend ...)` 原语，自动调 LLM 生成、验证、修复代码：

```scheme
(require std/llm all:)
(define __sp__ "Return ONLY Aura code. Always call (display ...).")
(define __gen__ (aura-make-gen __sp__))
(define __fix__ (aura-make-fix __sp__))

;; 一键生成并验证
(intend "Write (+ 1 2 3 4 5)" __gen__ aura-verify __fix__ 3)
;; → #(status:"ok" goal:"..." iterations:1)
```

`(intend ...)` 内部流程：

```
生成器 (http-post LLM) → 代码 → 验证器 (typecheck + eval)
  ↑                                        ↓
  └──── 修正器 (报错反馈给 LLM) ←──── 失败 ────┘
```

也支持通过 Python 脚本批量评测（85 个任务覆盖基础语法、标准库、类型系统、C FFI、TCP、LeetCode、ADT、线性所有权）：

```bash
# Python 手动修正循环（推荐）
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --rounds 3 --fix

# 原生 intend 原语（C++ 循环管理器，零 Python HTTP）
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --rounds 3 --intend

# 多模型自动对比
python3 tests/run_bench_all.py

# 只跑类型系统任务
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --tasks type-check,type-of,occurrence
```

两种模式均支持 `--fix`（迭代修正）和 `--max-attempts N`（最多尝试次数）。

---

## 九、M4 线性所有权

Aura 支持线性类型的所有权跟踪，用于资源管理：

```scheme
;; 声明线性类型变量
(define x : (Linear Int) 42)

;; 借用（不转移所有权）
(define y : (Linear Int) (& x))        ;; 借用 x
(define z : (Linear Int) (&mut-x))     ;; 可变借用

;; 移动 — 所有权转移
(define a (move x))                    ;; x 不再可用
```

### 读宏语法糖

```scheme
;; &x = (borrow x)
(& x)                ;; 不可变引用
(&mut-x)             ;; 可变引用

;; 编译期 OwnershipEnv 自动跟踪
;; 运行时检测 double-move / use-after-move
```

线性所有权当前阶段：编译期 `OwnershipEnv` 跟踪 + IR opcode (`LinearWrap`/`MoveOp`/`BorrowOp`/`DropOp`) + 运行时支持。

---

## 十、EDSL / AI Agent 开发

Aura 的核心差异化能力——代码自修改 AST + 增量编译，让 Agent 精确读写代码。

### 智能查询
```scheme
(set-code "(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))")

;; 查询影响范围
(query:def-use "fib")     → ((21) . (23 6 12))   ; (defs . uses)
(query:reaches 21)          → ((21) . (23 6 12))   ; 定义→所有使用点
(query:effects "fib")      → ((21) (23 6 12) (25 17 11))  ; + callers
```

### AST 编辑
```scheme
(mutate:rebind "fib" "(lambda (n) (* n 2))" "linearize")  ; 替换定义
(mutate:wrap 3 "(display _)" "wrap")                        ; 包裹表达式
(mutate:splice 0 1 "(display 1)" "(display 2)" "insert")  ; 批量插入
```

### 快照与回退
```scheme
(ast:snapshot "checkpoint")   ; 保存状态
(mutate:rebind "fib" "...")
(ast:restore 0)                ; 回退
(ast:diff 0)                   ; 行级结构化 diff
(ast:summary)                  ; AST 统计: 节点/类型/mutation
(compile:status)               ; 增量编译状态
```

### Workspace 分层
```scheme
(ws:create "sandbox")  (ws:switch 1)
(mutate:rebind "fib" "...")  (eval-current)
(ws:switch 0)                   ; 主代码完好
(ws:merge 1)                    ; 合并好的版本
(ws:lock 1 #t)                  ; 只读锁定
```

### 跨 Agent 通信
```scheme
;; Session A: (send "agent-b" "hello")
;; Session B: (my-id) → "agent-b", (recv 100) → "hello", (reply "pong")
```

### 代码合成
```scheme
;; 模板: (synthesize:fill "handler" "users")
;; LLM:  (synthesize:define "fib" "Int -> Int" :prompt "iterative")
;; 遗传: (synthesize:optimize "fib" :population 20)
;; 管线: (synthesize:pipeline "build" step1 step2 (rule:apply-all))
```

### 规范系统
```scheme
(require "std/rule" all:)
(rule:define 'guard-div :pattern "(/ ?x ?y)" :replace "(if (= ?y 0) 0 (/ ?x ?y))")
(rule:apply-all)   (rule:list-violations)   (rule:save "rules.json")
```

```lisp
;; 1. 设置代码
(set-code "(define (bad-fact n) (* n (bad-fac (- n 1))))")

;; 2. 定位问题 — 查找符号引用
(query:find "bad-fac")        ;; → (5)  函数体内的递归调用

;; 3. 自动修复 — 按函数名替换定义
(mutate:rebind "bad-fact"
  "(define (bad-fact n) (if (= n 0) 1 (* n (bad-fact (- n 1)))))"
  "fix typo")

;; 4. 获取修复后源码
(current-source)

;; 5. 验证
(eval-current)
(bad-fact 5)                  ;; → 120
```

EDSL API 完整列表：

| 原语 | 功能 |
|------|------|
| `set-code` | 设工作区源码（parse + set AST）|
| `current-source` | 取当前工作区源码字符串 |
| `eval-current` | 执行当前工作区代码 |
| `eval-current-output` | 执行工作区，返回 display 输出 + 值 |
| `typecheck-current` | 类型检查工作区代码 |
| `query:find` | 按符号名查节点 ID |
| `query:node-type` | 按节点类型过滤 |
| `query:node` | 获取节点详情（tag/value/type/sym-id）|
| `query:children` | 获取子节点 ID 列表 |
| `query:parent` | 查找父节点 ID |
| `query:calls` | 查哪些节点调用了某函数 |
| `query:pattern` | 结构模式匹配（`(+ 1 ...)` 等）|
| `query:type` | 查询节点的类型 |
| `mutate:rebind` | 按函数名替换完整定义 |
| `mutate:set-body` | 替换函数体（保留参数签名）|
| `mutate:replace-value` | 按节点 ID 替换值 |
| `mutate:replace-type` | 替换类型标注 |
| `mutate:remove-node` | 按节点 ID 断开子树 |
| `intend` | 循环编排（generator + verifier + [fixer] + [max-attempts]）|

---

## 十一、错误处理

```scheme
(try
  (/ 1 0)
  (catch (e) (display "oops")))

(raise "something wrong")
(assert (= 1 1))
```

---

## 十二、模块

```scheme
(require std/list)          ;; 带前缀: list:map, list:foldl
(import "std/list")          ;; 无前缀
(export map filter foldl)    ;; 声明公有接口
```

---

## 十三、AI 控制器 — PID 多轮修复

Aura 内置**控制论反馈循环**驱动 LLM 逐步修正代码到目标。控制器测量当前输出与目标的距离，选择粗粒度（完整重写）或细粒度（EDSL 定点修改）策略。

### 工作原理

```
LLM 生成 → Aura 编译运行 → 检查输出 → measure-distance()
                                       ↓
                                coarse / fine / putt
                                       ↓
                         temperature / API ref / trace
                                       ↓
                               LLM 再次生成...
```

### 运行

```bash
# 跑全部 85 个 EDSL 任务（自动重试 5 次）
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --max-attempts 5

# 指定模型
LLM_MODEL=minimax-m2.7 LLM_API_KEY="***" python3 tests/edsl_benchmark.py --max-attempts 5

# 指定任务
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --tasks is-anagram,hash-stats
```

### 多模型结果 (57 核心任务)

| 模型 | 通过率 | 耗时 |
|:----|:------:|:----:|
| 🥇 **Grok 4.3** | **57/57 (100%)** 🎯 | ~10min |
| 🥈 **DeepSeek v4 Flash** | **54/57 (94.7%)** | ~10min |
| 🥉 **MiniMax-M2.7** | **53/57 (93.0%)** | ~15min |

全部 85 个任务的完整 benchmark 详见 [benchmark.md](benchmark.md)。

### 架构

```
Python 编排层                           Aura CaaS
═══════════                           ═══════════

serve.exec(full_code)           ──→   ./aura --serve
  coarse / fine / putt                  compile + eval
  measure-distance                      json 响应

build_adaptive_feedback() → correction → LLM 再生成
```

详情 → [docs/benchmark.md](docs/benchmark.md) · [docs/roadmap.md](docs/roadmap.md)

---

**下一步:** `docs/roadmap.md` · `docs/design/aura_language_spec.md` · `docs/design/ffi_c.md` · `python3 tests/edsl_benchmark.py --tasks fibonacci`

## 多 Agent 编排（交响乐指挥模式）

Agent 编排框架将代码进化过程视为一场交响乐演奏：

```
指挥（orch:conduct） → 总谱 → 乐手们各司其职
```

### 基础用法

```scheme
(require "std/orchestrator" all:)
(require "std/string" all:)

;; 聘用乐手
(agent:spawn "planner"
  (lambda (task) (string-append "PLAN: " task)))
(agent:spawn "coder"
  (lambda (plan) (string-append "CODE: (" plan ")")))
(agent:spawn "tester"
  (lambda (code) (string-append "TEST: " code " => PASS")))

;; 指挥给提示
(agent:ask "planner" "fib" 30)    → "PLAN: fib"

;; 完整管线
(orch:pipeline (list "planner" "coder" "tester") "fib")
    → "TEST: CODE: (PLAN: fib) => PASS"

;; 并行执行
(orch:parallel
  (list (lambda (x) (+ x 1)) (lambda (x) (* x 2)))
  5)  → (6 15)
```

### 生命周期管理

```scheme
(agent:spawn "w" (lambda (x) (string-append "w:" x)))
(agent:status "w")           → "running"
(agent:stop "w")             → "stopped"
(agent:restart "w" (lambda (x) (string-append "v2:" x)))
(agent:ask "w" "task")       → "v2:task"
```

### AST 可视化

```scheme
(require "std/ast-viz" all:)
(ast:to-dot)  → digraph AST { Node0 [label="Define\nadd",fillcolor="#fccde5"]; ... }
(mutation:trace)  → "Total mutations: 1"
```
