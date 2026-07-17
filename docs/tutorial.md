# Aura 入门

语法像 Scheme，核心是**可被 Agent 查询、变异、进化的活 AST**。

```bash
./build.py build
echo '(+ 1 2 3)' | ./build/aura    # → 6
```

- 运行时全名表：`(api-reference)`（含 `*deprecated*` 段）
- 静态索引：[api-reference.md](api-reference.md) · [generated/primitives.md](generated/primitives.md)
- **Agent 一页提示词**：[agent-prompt-template.md](agent-prompt-template.md)

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
(require "std/surface" all:)   ;; string/json/math 产品面（P3）
(require "std/list" all:)
(map (lambda (x) (* x 2)) (list 1 2 3))   ;; → (2 4 6)

(require "std/json" all:)
(json-parse "{\"a\":1}")

(require "std/engine-metrics" all:)  ;; 观测 facade 包装
```

模块列表：`lib/std/*.aura` · 索引 [generated/stdlib-index.md](generated/stdlib-index.md)

---

## 核心自修改循环（规范名 — Issue #1438）

工作区模型：

`set-code` 锁定 AST → **`(query :op …)`** 导航 → **`(mutate :op …)`** 修改 → `eval-current` 执行。

```scheme
;; 1) 装入源码
(set-code "(define (f x) (+ x 1))")

;; 2) 执行当前工作区
(eval-current)                    ;; f 已绑定

;; 3) 结构查询（规范入口）
(query :find "f")                 ;; 按符号名找节点
(query :root)                     ;; 根节点 id
(query :children (query :root) :stable #t)  ;; (id . gen) — #393

;; 4) 变异（规范入口）
(mutate :rebind "f" "(lambda (x) (* x 2))" "double body")

;; 5) 再执行 — f 现在是 ×2
(eval-current)
(f 21)                            ;; → 42
```

### 观测（不要新增 query:*-stats）

```scheme
(engine:metrics)                         ;; schema 2 + compile/jit/… 分组
(engine:metrics :group "jit")
(engine:metrics "query:pattern-stats")   ;; 过渡期按名单取
(require "std/engine-metrics" all:)
(engine-metrics:get)
```

### Agent 闭环（Issue #1460 — 默认 EDSL，非纯 LLM）

```scheme
(require "std/agent" all:)

;; 决策指标（#1461 最小契约）— 每次 commit 前必读
(agent:decision-metrics)   ; → hash schema 1461 + recommendation
(agent:decide)             ; → 'commit | 'back-off | 'escalate

;; 默认路径：query → decide → mutate:atomic-batch → eval → 再 decide
(auto-grow "double f"
  :source "(define (f x) (+ x 1))"
  :rebind "f" "(lambda (x) (* x 2))")

;; 单轮可测入口
(agent:closed-loop-once
  :source "(define (f x) (+ x 1))"
  :rebind "f" "(lambda (x) (* x 2))")

;; 旧 LLM-only 路径（兼容，非默认）
(auto-grow "write a fact function" :prompt-only)
```

详见 [design/agent-decision-metrics.md](design/agent-decision-metrics.md) · [agent-prompt-template.md](agent-prompt-template.md)。

### 快照与回滚

```scheme
(define snap (ast:snapshot "checkpoint"))
(mutate :rebind "f" "(lambda (x) 0)" "try")
(if (not (= 42 (begin (eval-current) (f 21))))
    (ast:restore snap))
```

### Workspace 分层（规范入口）

```scheme
(workspace :create "sandbox")     ;; → id
(workspace :switch 1)
(mutate :rebind "f" "(lambda (x) (* x 3))" "sandbox try")
(workspace :switch 0)
(workspace :merge 1)
(workspace :lock 1)
(workspace :unlock 1)
```

### 旧名（deprecated，仍可用）

| 旧 | 新（优先） |
|----|------------|
| `(query:find "f")` | `(query :find "f")` |
| `(mutate:rebind …)` | `(mutate :rebind …)` |
| `(workspace:create …)` | `(workspace :create …)` |
| `(query:foo-stats)` | `(engine:metrics "query:foo-stats")` |

完整 deprecated 列表见运行时 `(api-reference)` 的 `*deprecated*` 段。

更多：`tests/suite/mutate-structured.aura` · `tests/test_query_dispatch.cpp` · `tests/test_mutate_dispatch.cpp`

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

### Multi-fiber 并行（生产路径）

```scheme
;; 真正的多 fiber 批处理 — 并发 cap + timeout + 结果聚合
(define batch
  (parallel-intend
    (vector (lambda () 1) (lambda () 2) (lambda () 3))
    :max-concurrency 4
    :timeout-ms 10000
    :fail-fast #f))
(hash-ref batch "ok-count")   ; → 3
(hash-ref batch "status")     ; → "ok"
```

完整教程（背压、错误处理、观测、C++ `src/orch/`）：
[orchestration-tutorial.md](orchestration-tutorial.md) · 压力套件
`tests/suite/parallel_orchestration_stress.aura`

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
| Agent 一页提示 | [agent-prompt-template.md](agent-prompt-template.md) |
| API 表 | [api-reference.md](api-reference.md) |
| 模块结构 | [architecture.md](architecture.md) |
| 改运行时 | [contributing.md](contributing.md) |
| JSON 协议 | [wire-formats.md](wire-formats.md) |
| 路线图 | [roadmap.md](roadmap.md) |
