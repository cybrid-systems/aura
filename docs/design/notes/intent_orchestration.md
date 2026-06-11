# Intent Orchestration — 设计文档

> 为 AI Agent 设计的高层意图原语。
> 目标：LLM 说"要什么"，系统自动拆解为 EDSL 操作序列并迭代完成。

**更新：2026-05-23**
**状态：E1-E4 已完成，E5 远期计划**

---

## 现状：手动的两阶段管线

```
┌─────────────────────────────────────────────────┐
│  LLM 生成代码  →  Aura 编译器  →  成功/失败       │
│                                              │
│  失败时：                                       │
│  set-code → query 定位 → mutate 修复 →         │
│  typecheck → eval → current-source → 反馈给 LLM │
└─────────────────────────────────────────────────┘
```

所有编排是硬编码的（`ai_agent_edsl.py`、`--fix` 循环）。每加一个新场景就要写新流程。

## 设计目标

1. **LLM 说"要什么"，不说"怎么做"**
2. **意图拆解策略本身可演化**（像代码一样可以 mutate）
3. **对 benchmark 友好** — 评测从"LLM 写对的代码"变成"intend 是否达成目标"
4. **薄** — 不是重引擎，是一个内置原语

---

## 1. `intend` 原语

### 语法

```scheme
(intend goal-expr
  strategy: strategy-name
  max-attempts: n
  timeout: seconds
  verbose?: bool)

; 示例
(intend "make fib tail-recursive"
  strategy: refactor-to-tail-call
  max-attempts: 3)

(intend "fix compilation errors"
  strategy: error-feedback-loop
  max-attempts: 5)

(intend "implement merge-sort"
  strategy: generate-and-fix
  max-attempts: 10)
```

### 返回值

```scheme
; 成功
'#(status: "ok"
   goal: "make fib tail-recursive"
   code: "(define (fib n) ...)"        ; 最终代码
   iterations: 3                        ; 迭代次数
   timeline: (...))                     ; 操作时间线

; 失败（超时或用完 attempts）
'#(status: "failed"
   goal: "..."
   iterations: 5
   last-error: "parse error at line 3"
   partial-code: "...")                 ; 最后一次尝试的代码
```

### 语义

1. `goal-expr` 被传给 LLM（作为"你想做什么"的意图）
2. 系统根据 `strategy` 拆解为 EDSL 操作管线
3. 执行管线，每步记录到 timeline
4. 如果某步失败 → 根据 strategy 决定：修正 / 回滚 / 换策略
5. 如果所有步完成但目标未达成 → 反馈给 LLM，开启下一轮迭代
6. 达到 `max-attempts` 或 `timeout` 停止

---

## 2. 策略（Strategy）系统

策略是**纯 Aura 定义的可演化的 S-表达式**，不是硬编码的。

### 内置策略

```scheme
; ── 1. 生成 + 修正（最通用）────────────────────────
; 适用：写新函数、实现算法
; 流程：LLM 生成初稿 → eval → 报错？→ 修正循环
(define-strategy generate-and-fix
  steps:
    [(phase "generate"
       action: (llm-generate goal))
     (phase "verify"
       action: (begin
                 (typecheck-current)
                 (eval-current)))
     (phase "fix" :on-error
       action: (begin
                 (set-code (llm-fix (current-source) last-error))
                 (recur)))])           ; → verify

; ── 2. 重构（修改现有代码）────────────────────────
; 适用：尾递归优化、重命名、提取函数
(define-strategy refactor
  steps:
    [(phase "locate"
       action: (query:find target-name))
     (phase "mutate"
       action: (mutate:rebind target-name new-body))
     (phase "verify-semantics"
       action: (begin
                 (eval-current test-case)
                 (= result expected)))]])

; ── 3. 调试修复（最常用的 --fix 模式）──────────────
; 适用：编译错误、类型错误、运行时结果不符
(define-strategy error-feedback-loop
  steps:
    [(phase "check"
       action: (typecheck-current))
     (phase "run"
       action: (eval-current))
     (phase "feedback" :on-error
       action: (let ((err (last-error))
                     (src (current-source)))
                 (llm-fix src err expected-goal)))
     (phase "retry" :on-changed
       action: (recur))])

; ── 4. 优化（性能导向）────────────────────────────
; 适用：添加类型标注、开启 JIT、常量折叠
(define-strategy optimize
  steps:
    [(phase "profile"
       action: (benchmark-function target))
     (phase "suggest"
       action: (llm-suggest-optimization target profile))
     (phase "apply"
       action: (mutate:set-body target suggestion))
     (phase "verify"
       action: (eval-current test-case))
     (phase "bench" :if-passed
       action: (benchmark-function target))])
```

### `define-strategy` 宏

```scheme
(defmacro define-strategy (name . clauses)
  "定义策略。展开为一个可 mutate 的 strategy record。"
  `(begin
     (define ,name
       (make-strategy
         ',(symbol->string 'name)
         (quote ,clauses)))
     (register-strategy! ,name)))

; 策略是普通值——可以被 query、被 mutate、被传递：
(query:find "strategy")
; → (2)  ; 策略本身在 AST 中有节点

(mutate:rebind "error-feedback-loop" new-definition)
; → #t  ; 运行时修改策略定义
```

---

## 3. `intend` 的执行模型

```
意图
  │
  ▼
┌─────────────────────────┐
│  Strategy Resolver       │  ← 查 strategy 定义
│  (展开 steps)            │
└─────────┬───────────────┘
          │
          ▼
┌─────────────────────────┐
│  Step Runner             │  ← 逐个执行 phase
│                          │
│  ┌───┐  ┌───┐  ┌───┐   │
│  │ S1│→│ S2│→│ S3│→... │
│  └───┘  └───┘  └───┘   │
│                          │
│  每步可选：               │
│  · on-error → 修正       │
│  · on-changed → 重试     │
│  · if-passed → 后续      │
└─────────┬───────────────┘
          │
          ▼
┌─────────────────────────┐
│  Timeline Recorder       │  ← 记录每步结果
│  (AST + 结果 + 时间)     │
└─────────┬───────────────┘
          │
          ▼
    成功 / 失败 → LLM 判断 → 下一轮迭代
```

### Timeline 记录

每次 `(intend ...)` 执行产生一个不可变的时间线（append-only），可以回放：

```scheme
(intend-history)
; → (#[iteration: 1
;     step: "generate"
;     action: (llm-generate "make fib tail-recursive")
;     result: (define (fib n) ...)
;     time: 2.3s]
;    #[iteration: 1
;     step: "verify"
;     action: (eval-current)
;     result: error: "stack overflow"
;     time: 0.1s]
;    #[iteration: 1
;     step: "fix"
;     action: (llm-fix ... "stack overflow")
;     result: (define (fib n) ...)
;     time: 3.1s]
;    ...)
```

---

## 4. 与现有系统的关系

### 不取代，而是组合

```
         ┌──────────────┐
         │   intend      │  ← 新（高层编排）
         └──────┬───────┘
                │  拆解为
         ┌──────▼───────┐
         │   EDSL        │  ← 现有（原子操作）
         │ query / mutate│
         │ typecheck/eval│
         └──────────────┘
```

`intend` 不取代 query/mutate/typecheck/eval——它把它们串成管线的语法糖，加上自动错误处理和迭代。

### 对 `--fix` 的关系

当前 `--fix` 是 `intend` 的一个特例：

```scheme
; 等价于 --fix 的全部逻辑：
(intend goal
  strategy: error-feedback-loop
  max-attempts: N
  verbose?: #t)
```

### 对 edsl_benchmark 的关系

Benchmark 任务从"LLM 一次写对"变成评估意图系统的成功率：

```python
# 之前：测 LLM 单次代码生成质量
success = llm_generate("Write merge sort") → run → check

# 之后：测 intend 的达成率
success = aura_eval(f'(intend "Write merge sort")') → check
```

这更符合 Aura 的设计哲学——LLM 可以试错，关键是系统能否在迭代中成功。

---

## 5. 实现路径

### Phase 1: `intend` 原语（2-3 天）✅

- 在 `src/compiler/evaluator_impl.cpp` 注册 `(intend goal [max-attempts])` 为内置原语
- 内置 generate-and-fix 循环：
  - LLM 生成代码 → `parse_to_flat` + `eval_flat` 编译验证
  - 报错 → 构建 correction prompt → 喂回 LLM（使用内置 `json-encode`/`json-get-string` 原语，零手工转义）
  - 重复至 max-attempts 次
- 配置通过环境变量：`LLM_API_KEY`、`LLM_MODEL`、`LLM_BASE_URL`
- LLM 调用通过 `curl` + JSON API（复用 `http-post` 相同模式）
- 返回 `"#(status:ok/failed goal:... iterations:...)"` 字符串
- 无 API key 时优雅返回失败，不崩溃
- 注册在 `service.ixx` 的 tree-walker-only EDSL 列表
- 7 个边界测试：空参数、空 goal、max-attempts=0/5、EDSL 管线集成
- 文件：`src/compiler/evaluator_impl.cpp` (+199行)、`src/compiler/service.ixx`、`tests/test_intent.aura`

**当前状态 (E4 Phase 1-3 完成):**
- 结构化 intend-history ✓ — 14 字段 IntendRecord + 滑动窗口
- intend-analytics 原语 ✓ — 按策略/任务聚合统计
- 错误类型分类 ✓ — 8 种错误模式自动标注
- strategy-field / strategy-set-field! ✓ — 策略字段读写
- evolve-strategy ✓ — 根据错误模式追加针对性提示
- benchmark --evolve ✓ — hints 自动注入下一轮 system prompt
- 闭包捕获 bug 修复 ✓ — memoize 任务 47/47 通过

**待办：**
- Phase 4: 多意图协作与意图树（远期）
- `build.py` fuzz 测试套件（见 llm-fuzz 设计文档）
- 验证器升级：不只验代码能跑，还要验输出匹配期望值

### Phase 2: Strategy 系统（3-5 天）

- `define-strategy` 宏
- 内置 3-4 个策略（generate-and-fix, error-feedback-loop, refactor, optimize）
- Timeline 记录 + history 查询
- 测试：每个策略的端到端管线

### Phase 3: 集成（2-3 天）

- 用 `intend` 替换 `edsl_benchmark.py` 的 `--fix` 循环
- Benchmark 报告自动包含 iteration 数、strategy 名、timeline
- 评测从"代码正确率"变成"意图达成率"

### Phase 4: 可演化策略（远期）

- LLM 通过 `(mutate:rebind "strategy-name" new-def)` 修改策略本身
- 多意图协作：`(intend A)` 内部调 `(intend B)`
- 意图树可视化

---

## 6. 关键设计决策

### 为什么策略是 S-表达式而不是 Python/C++？

1. **普通值**：可以被 query、mutate、eval——和 Aura 的其他数据结构一样
2. **可演化**：LLM 可以通过 EDSL 修改策略自身，不需要重新编译
3. **自洽**：`define-strategy` 展开后存为 AST 节点，timeline 也是 AST 节点

### 为什么不是"意图引擎"？

"引擎"暗示一个外部系统。Aura 的哲学是一切都是代码——intent 是原生原语，不引入外部依赖，不新增 DSL。LLM 学一个原语 `(intend ...)` 比学一套引擎 API 容易得多。

### 为什么和 strategy 分开？

Goal（做什么）和 strategy（怎么做）分离，因为同一个 goal 可以用不同策略试：

```scheme
; 先用 generate-and-fix 试
(intend "quicksort" strategy: generate-and-fix)

; 不行换 refactor（从现有实现改过来）
(intend "quicksort" strategy: refactor resource: "bubblesort")

; 再不行换 optimize（用 std/list sort 改）
(intend "quicksort" strategy: optimize resource: "std/list")
```

---

## 7. 示例：完整的 intend 会话

```scheme
; 用户/LLM 发一个意图
(intend "Write a function that takes a list and returns unique elements"
  strategy: generate-and-fix
  max-attempts: 3)

; 系统输出 timeline：
iteration 1:
  step generate: LLM 生成
    (define (unique lst)
      (if (null? lst) ()
        (cons (car lst)
              (unique (filter (lambda (x) (not (= x (car lst)))) (cdr lst))))))

  step verify: error — "unbound variable: filter"
  
  step fix: LLM 看到错误，修正
    (require std/list all:)
    (define (unique lst)
      (if (null? lst) ()
        (cons (car lst)
              (unique (filter (lambda (x) (not (= x (car lst)))) (cdr lst))))))
  
  step verify: 输出 (1 2 3 4) 但传递顺序是嵌套 list
  
iteration 2:
  step generate: LLM 重新生成（看到预期输出和实际输出的差异）
    (require std/list all:)
    (require std/hash all:)
    (define (unique lst)
      (hash-keys (let loop ((acc (hash)) (rest lst))
                   (if (null? rest) acc
                     (loop (hash-set! acc (car rest) #t) (cdr rest))))))
  
  step verify: 输出 (1 2 3 4) ✅

; 最终返回：
'#(status: "ok"
   goal: "unique elements"
   code: "(require std/list all:) (require std/hash all:) ..."
   iterations: 2
   timeline: (...))
```

---

## 相关文档

- [Query + Transform EDSL](query_edsl.md) — 底层 AST 操作原语
- [FlatAST 设计](ast_validate.md) — AST 数据结构
- [edsl_benchmark.py](../../tests/edsl_benchmark.py) — 评测框架
- [roadmap.md](../roadmap.md) — 项目路线图
