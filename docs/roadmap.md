# Aura 路线图

**更新：2026-05-25 — 对齐当前实现，移除已完成项**

---

## ✅ 已完成

| Phase | 内容 | 状态 |
|:------|:-----|:----:|
| W1-2 | `query:def-use` / `query:reaches` / `query:effects` | ✅ |
| W1-2 | `ast:snapshot` / `ast:restore` / `ast:diff` | ✅ |
| W1-2 | `ast:summary` / `compile:status` | ✅ |
| W3-4 | `mutate:splice` / `mutate:wrap` / `mutate:refactor/extract` | ✅ |
| W3-4 | `query:root` / workspace root stable node ID | ✅ |
| W5-6 | `workspace:create` / `switch` / `list` / `delete` / `lock` / COW | ✅ |
| W5-6 | `workspace:sync-from` / `workspace:merge` / `workspace:discard` | ✅ |
| W7-8 | `send` / `recv` / `my-id` / `reply` / `session-active?` / `mailbox-count` | ✅ |
| W9-10 | `synthesize:register-template` / `synthesize:fill` / `synthesize:define` (LLM) | ✅ |
| W9-10 | `synthesize:pipeline` / `synthesize:optimize` (genetic) | ✅ |
| W9-10 | `rule:define` / `rule:apply` / `rule:save` / `rule:load` / scope/condition | ✅ |

---

## 🔴 迭代 P1 — Type System 修复（预计 5-7d）

目标：消除类型系统相关的稳定失败，双模型提分

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

### P2a: 标准库运行时适配（2d）

std/rule 和 std/pipeline 模块代码已存在，但运行时缺少关键字参数和部分原语支持，导致模块不可用。

| # | 任务 | 工时 | 问题 |
|:-:|:-----|:----:|:-----|
| 1 | **运行时关键字参数支持** | 1-2d | `:description`, `:workspace` 等关键字无法作为原语使用；rule:define/pipeline 依赖的 try/catch 可能也不完整 |
| 2 | **`string-append` + 字符串拼接一致性** | 0.5d | 模块代码大量使用 `string-append`，需验证多参数形式在所有路径下工作 |

### P2b: EDSL API 修复（2-3d）

| # | 任务 | 影响任务 | 根因 |
|:-:|:-----|:---------|:-----|
| 1 | **`mutate:replace-value` + `eval-current` 不一致** | edsl-mutation-rollback | 修改后 eval 不反映 mutation |
| 2 | **`ast:snapshot` / `ast:list-snapshots` 持久化** | edsl-snapshot-multi | snapshot 只在内存，`list-snapshots` 返回空 |
| 3 | **`workspace:switch` + COW 隔离** | edsl-workspace-cow | 子 workspace 隔离没有生效，写操作泄露 |
| 4 | **`edsl-require-stdlib` 语法** | edsl-require-stdlib | `(require 'std/... all:)` 语法解析不完整 |
| 5 | **`synthesize:pipeline` — invalid node id** | edsl-synthesize-pipeline | 模板填充后 AST node id 不连续，pipeline 执行时触发 |

---

## 🟢 迭代 P3 — FFI + 算法任务 Hints（预计 2d）

目标：跨过最后 ~6 个 LLM 能力上限 + 2 个 FFI 失败

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
| P3 | AOT 布尔值输出 raw int（`1` 而非 `#t`） | 1d | untagged runtime 无法区分 #t 和 1 |
| P3 | struct 模块 AOT 不工作 | 2d | define-type 在 IR 路径不被处理 |
| P3 | `display` 嵌套对/improper list 格式化 | 1d | 和 eval 一致但可改进 |
| P3 | messaging 阻塞 recv | 2d | 单线程 serve 无法真正阻塞 |
| P3 | workspace tree 跨 session 共享 | 2d | 每个 serve session 独立 workspace tree |
| P3 | `synthesize:optimize` fitness 基于 benchmark | 1d | 当前仅基于代码长度 |
| P3 | 规则 VCS 集成 | 2d | 当前仅 JSON 文件持久化 |

## 🔭 前瞻 (Q3-Q4 2026)

| 任务 | 说明 |
|:-----|:------|
| VS Code / Cursor 插件 | 原生 EDSL 支持 |
| LSP / 包管理 / 自举 | 独立长期项目 |
| 分布式 EDSL | 多机共享 AST workspace |
| 形式化证明接口 | 关键 mutate 走 proof 而非 test |
| LangGraph / CrewAI 集成 | 适配主流 Agent 框架 |
