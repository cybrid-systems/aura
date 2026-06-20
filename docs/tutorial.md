# Aura 入门

语法像 Scheme，核心是**可被 Agent 查询、变异、进化的活 AST**。

```bash
./build.py build
echo '(+ 1 2 3)' | ./build/aura    # → 6
```

原语全貌：`(api-reference)` 或见 [api-reference.md](api-reference.md)。

**Agent 推荐入口**：`./build/aura --serve-async` + [wire-formats.md](wire-formats.md)

---

## 基础

```scheme
(+ 1 2 3)                    ;; → 6
(define (square x) (* x x))
(let ((x 10) (y 20)) (+ x y)) ;; → 30

(if (< 2 3) "yes" "no")      ;; → "yes"
(list 1 2 3)
(hash-ref (hash "a" 1) "a")  ;; → 1
```

类型反射：`(type-of 42)` → `"Int"` · `(integer? 42)` → `#t`

---

## 标准库

```scheme
(require "std/list" all:)
(map (lambda (x) (* x 2)) (list 1 2 3))   ;; → (2 4 6)

(require "std/json" all:)
(json-parse "{\"a\":1}")

;; 模块列表见 lib/std/*.aura
```

---

## 自修改 EDSL（核心）

工作区模型：`set-code` 锁定 AST → `query:*` 导航 → `mutate:*` 修改 → `eval-current` 执行。

```scheme
(set-code "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))")

(query:find "fact")           ;; 按名查节点
(query:pattern "(fact ...)")  ;; 模式匹配

(mutate:rebind "fact"
  "(define (fact n)
     (let loop ((n n) (acc 1))
       (if (= n 0) acc (loop (- n 1) (* acc n)))))"
  "迭代化")

(eval-current)
(fact 10)                     ;; → 3628800
```

### 快照与回滚

```scheme
(define snap (ast:snapshot "checkpoint"))
(mutate:rebind "fact" "..." "try")
(if (not (= 120 (fact 5)))
  (ast:restore snap))
```

### Workspace 分层

```scheme
(workspace:create "sandbox")
(workspace:switch 1)
(mutate:rebind "fact" "...")
(workspace:switch 0)
(workspace:merge 1)
```

更多：`tests/suite/mutate-structured.aura` · `tests/suite/edsl_errors.aura`

---

## Agent 编排

```scheme
(require "std/orchestrator" all:)

(orch:define-role "coder"
  (orch:role (lambda (task) (string-append "[CODE] " task))))

(orch:pipeline (list "planner" "coder" "tester") "build fib")

(orch:parallel
  (list
    (lambda (x) (string-append "style: " x))
    (lambda (x) (string-append "perf: " x)))
  "snippet")
```

测试：`tests/suite/orchestrator.aura` · 实现：`lib/std/orchestrator.aura`

---

## C FFI（摘要）

```scheme
(define lib (c-load "libm.so.6"))
(define sqrt (c-func lib "sqrt" 2 2))
(sqrt 9.0)   ;; → 3.0
;; 类型: 1=Int 2=Float 3=String 4=Opaque 0=Void
```

---

## LLM 与 benchmark

```scheme
(require "std/llm" all:)
;; (intend "goal" generator verifier fixer max-attempts)
```

```bash
LLM_API_KEY="..." python3 tests/edsl_benchmark.py --rounds 3
./build.py test bench
```

数据：[benchmark.md](benchmark.md)

---

## 下一步

| 目标 | 文档 |
|------|------|
| 模块结构 | [architecture.md](architecture.md) |
| 改运行时 | [contributing.md](contributing.md) |
| JSON 协议 | [wire-formats.md](wire-formats.md) |
| 路线图 | [roadmap.md](roadmap.md) |