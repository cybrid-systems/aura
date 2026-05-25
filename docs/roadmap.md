# Aura 路线图

**更新：2026-05-25 — EDSL Benchmark 根因分析后迭代计划重排**

---

## ✅ 已完成 — 核心 EDSL & Agent 交互能力

| Phase | 内容 | 状态 |
|:------|:-----|:----:|
| W1-2 | `query:def-use` / `query:reaches` / `query:effects` | ✅ |
| W1-2 | `ast:snapshot` / `ast:restore` / `ast:diff` | ✅ |
| W1-2 | `ast:summary` / `compile:status` | ✅ |
| W3-4 | `mutate:splice` / `mutate:wrap` / `mutate:refactor/extract` | ✅ |
| W5-6 | `workspace:create` / `switch` / `list` / `delete` / `lock` / COW | ✅ |
| W7-8 | `send` / `recv` / `my-id` / `reply` / `session-active?` / `mailbox-count` | ✅ |
| W9-10 | `synthesize:register-template` / `synthesize:fill` / `synthesize:define` (LLM) | ✅ |
| W9-10 | `synthesize:pipeline` / `synthesize:optimize` (genetic) | ✅ |
| W9-10 | `rule:define` / `rule:apply` / `rule:save` / `rule:load` / scope/condition | ✅ |
| W5-6 | `workspace:sync-from` / `workspace:merge` / `workspace:discard` | ✅ |

---

## 🔴 迭代 P1 — Type System 修复（预计 5-7d）

目标：消除 12 个类型系统相关的稳定失败，双模型提分至 ~105/132 (80%)

| # | 任务 | 工时 | 影响任务 | 根因 |
|:-:|:-----|:----:|:---------|:-----|
| 1 | **Occurrence Typing 对 cond / match 收窄修复** | 1-2d | type-occ-cond, type-occ-deep, type-occ-match, adt-either, m4-borrow-chain | cond 通过 if 脱糖但不触发 occ 收窄；match 模式变量作用域与 occ 结合有 bug |
| 2 | **Blame 运行时输出格式修正** | 0.5d | type-blame-runtime | `(+ 1 "hello")` 触发的 blame 信息 `<unknown>` — 需要输出谁 blame 谁 |
| 3 | **`symbol->string` / `string->number` 等转换原语补全** | 0.5d | type-coercion-chain, type-grad-multi-boundary | 缺基础原语，`(+ 42 "hello")` 的 coercion 需要这些中间函数 |
| 4 | **`:Int` 标注语法 Bug** | 1d | type-gradual-boundary | `(: x Int 42)` 形式 tokenizer/parser 解析不了 |
| 5 | **标注影响语义 — annotation erasure 不彻底** | 1d | type-gradual-erasure | 带 `: Int` 和不带 `: Int` 的执行路径不同，输出 `(5 5)` 而不是 `42` |
| 6 | **Let-polymorphism + HOF — value restriction 过严格** | 1d | type-let-poly-hof | `let` 绑定 lambda 时 `is_syntactic_value` 判断不对，导致不泛化 |
| 7 | **Linear HOF — move 语义 + closure 交互** | 0.5d | type-linear-hof | `(move (f x))` 在 HOF 场景下 move 了不该回收的值 |
| 8 | **Borrow 跨作用域运行时错误** | 0.5d | m4-borrow-chain | borrow 结束后 move — 运行时标记清理不对 |

---

## 🟡 迭代 P2 — Stdlib Gaps & EDSL API 补全（预计 4-5d）

目标：消除 ~12 个 EDSL 和标准库相关失败，提分至 ~117/132 (89%)

### P2a: 标准库补全 (2d)

| # | 任务 | 工时 | 影响任务 |
|:-:|:-----|:----:|:---------|
| 1 | **实现 `std/rule` 模块** | 1d | edsl-rule-basic, edsl-rule (2个 stable fail) |
| 2 | **实现 `std/pipeline` 模块** | 0.5d | edsl-pipeline-basic, edsl-synthesize-pipeline |
| 3 | **Hash API 统一命名 — 补 `make-hash` 别名** | 0.5d | two-sum, word-freq, unique-hash |

### P2b: EDSL API 修复 (2-3d)

| # | 任务 | 影响任务 | 根因 |
|:-:|:-----|:---------|:-----|
| 4 | **`query:def-use` 返回格式修复** | edsl-defuse, edsl-defuse-cross, edsl-defuse-multi | 返回 closure 而非 `((def-node-id...) . (use-node-id...))` |
| 5 | **`mutate:splice` / `wrap` 索引计算错误** | edsl-splice-wrap | splice 参数解析或 AST 索引计算不对 |
| 6 | **`mutate:replace-value` + `eval-current` 不一致** | edsl-mutation-rollback | 修改后 eval 不反映 mutation |
| 7 | **`ast:snapshot` / `ast:list-snapshots` 持久化** | edsl-snapshot-multi | snapshot 只在内存，`list-snapshots` 返回空 |
| 8 | **`workspace:switch` + COW 隔离** | edsl-workspace-cow | 子 workspace 隔离没有生效，写操作泄露 |
| 9 | **`my-id` / `session-active?` / `mailbox-count` 输出格式** | edsl-messaging | 返回 `edsl-messaging #t 0` 格式不符合预期 |
| 10 | **`edsl-require-stdlib` syntax** | edsl-require-stdlib | `(require 'std/... all:)` 语法解析不完整 |
| 11 | **`synthesize:pipeline` — invalid node id** | edsl-synthesize-pipeline | 模板填充后 AST node id 不连续 |

---

## 🟢 迭代 P3 — FFI + 算法任务 Hints（预计 2d）

目标：跨过最后 ~6 个 LLM 能力上限 + 2 个 FFI 失败，提分至 ~123/132 (93%)

| # | 任务 | 影响任务 | 方案 |
|:-:|:-----|:---------|:-----|
| 1 | **c-func 用 -1 (RTLD_DEFAULT) 的 TASK_HINT 强化** | ffi-sqrt | 当前 hint 不直接，LLM 持续用 0 |
| 2 | **c-func 调用模式示例 — 显示完整 (display (c-func ...))** | ffi-strlen | LLM 只写 `(c-func ...)` 不调用 |
| 3 | **tcp-connect 增加 timeout hint / mock 模式** | tcp-connect | 对外部服务依赖做本地 mock |
| 4 | **palindrome / sieve 增加完整工作示例** | palindrome, sieve | 给近乎完整的 Aura 代码 |
| 5 | **table-lookup / list-flatten / bench-parse 加更多测试检查点** | table-lookup, list-flatten, bench-parse | 当前预期值容易被 LLM 猜错方向 |
| 6 | **adt-either / adt-option pattern match hint** | adt-either, adt-option | 明确 match 语法细则 |

---

## 🚧 生产可用（待定）

| 优先级 | 任务 | 工时 | 说明 |
|:------:|:-----|:----:|:-----|
| P3 | 权限模型（module / symbol whitelist） | 3d | mutation 只允许特定 scope |
| P3 | eval 资源限制（CPU / memory / recursion depth） | 3-4d | serve 模式安全沙箱 |
| P3 | `ast:explain-diff` — 自然语言解释变更 | 2d | 结构化 diff → 文本 |
| P3 | `compile:timeline` — 编译事件日志 | 2d | 增量编译审计轨迹 |
| P3 | 交互式 AST browser (REPL mode) | 2-3d | 树形浏览 |
| P3 | 并行查询 / 增量 typecheck | 3-5d | 不阻塞 eval |

---

## 🔭 前瞻 (Q3-Q4 2026)

| 任务 | 说明 |
|:-----|:------|
| VS Code / Cursor 插件 | 原生 EDSL 支持 |
| struct 模块 AOT | `define-type`(EDSL)，IR 路径 |
| LSP / 包管理 / 自举 | 独立长期项目 |
| 分布式 EDSL | 多机共享 AST workspace |
| 形式化证明接口 | 关键 mutate 走 proof 而非 test |
| LangGraph / CrewAI 集成 | 适配主流 Agent 框架 |
