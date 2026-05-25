# Aura 路线图

**更新：2026-05-25 (AOT 完整管线完成)**

---

## AOT 测试覆盖：56 emit 全部通过

```
算术:    + - * / 链式
比较:    = < > <= >=
逻辑:    and or not
类型:    pair? null?
对:      cons car cdr (含 improper list)
列表:    list length list-ref reverse append member
         map filter foldl apply named-let
字符串:  string-length string=? string-append string-ref
         string<? number->string string->number
条件:    if let
闭包:    lambda closure (含自引用递归 + 环境捕获)
原语:    作为闭包值传递 (+ - * / = < > <= >= not)
所有权:  drop move borrow linear
高阶:    map filter foldl append member apply permutations
IO:      display (列表/嵌套/improper 格式化)
多文件:  ./aura --emit-binary a.aura b.aura out

stdlib 已验证:
  algorithm: sorted? merge-sorted binary-search unique combinations
             permutations sort-stable min-by max-by
  list:      map filter foldl range take drop
  math:      factorial sin cos (需 -lm 链接)
  maybe/csv/set/queue: 基本导入编译 ✅
```

## 技术要点

**编译管线：**
```
源码 → FlatAST → IRModule → FlatFunction
  → LLVM IR (O2) → .ll → llc -filetype=obj → .o
  → 链接 runtime.c → 独立 ELF
```

**关键修复：**
- 原语派发表: null?/pair?/cons/car/cdr/length/reverse/append/member/map/filter/foldl/list/display
- aura_closure_call: 创建 locals 数组填充 env[0..N] + args[0..N]
- 函数名唯一化: `__lambda__` → `__lambda___0`, `__lambda___1`, ...
- import 内联: 绕过 cache_module 保守 FnCheck → 模块源码直接拼接到输入
- apply: 遍历参数列表收集元素后调用 aura_closure_call

## 剩余工作

| 优先级 | 任务 | 说明 |
|:------:|:-----|------|
| 🟢 | struct 模块 AOT | 使用 `define-type` (EDSL)，IR 路径不支持 |
| 🟢 | LSP / 包管理 / 自举 | 独立长期项目 |
| 🟢 | 性能优化 (O3/LTO) | 功能完整后再做 |

## 已完成

- P0 (05-23): 核心求值 + 类型系统 + ADT + EDSL
- P1 (05-23): IR + Pass Manager + 增量编译
- P2 (05-23~25): JIT + 真实 AOT 编译器 + stdlib 集成

---

# EDSL & Agent 交互能力路线图

**评估基准：2026-05-25 代码，Self-modifying AST + Incremental Compilation + Query/Mutate API**

当前 EDSL 核心闭环已成熟：读取 → 查询定位 → 精细变异 → 增量 eval → 反馈。下一阶段重点从"单 Agent 精细编辑"演进到"多 Agent 协作治理"，同时强化可靠性和可观测性。

## 阶段一：地表强化（Week 1–4）

**目标**：补上 Agent 自主决策最缺的"看懂代码 + 记住历史"，零外部依赖。

### Week 1–2：语义查询 + 变更追踪

| # | 任务 | 工时 | 说明 |
|:-:|:-----|:----:|:-----|
| 🔴 P0 | `query:def-use` / `query:reaches` / `query:callers` | 3–5d | 静态单赋值追踪，复用已有 IR |
| 🔴 P0 | `ast:snapshot` / `ast:restore` / `ast:diff` | 2–3d | 在 MutationTransaction 上加快照栈 |
| 🔴 P0 | `ast:summary` / `compile:status` | 2d | AST 结构统计 + 增量编译脏/洁标记 |

**里程碑：** Agent 能回答"改 X 会影响谁？"；可在事务里 safe evaluate → rollback → 继续。

### Week 3–4：基础可视化 + 结构化变换

| # | 任务 | 工时 | 说明 |
|:-:|:-----|:----:|:-----|
| 🔴 P0 | `ast:visualize` → Graphviz DOT / Mermaid | 2d | 复用已有 IR 图 |
| 🟡 P1 | `mutate:insert` / `mutate:splice` / `mutate:map` | 3–5d | 基于 query:find/pattern 定位后插入 |
| 🟡 P1 | `mutate:refactor` / `mutate:inline` / `mutate:wrap` | 3d | 高阶结构化变换 |
| 🟡 P1 | `synthesize:fill`（填空式合成） | 2d | 复用 intend 基础设施 |

**里程碑：** LLM 可以说"把所有 Int→Int64"而非逐个坐标操作。

**阶段一验收：** Agent 能 **查询影响 → 做复杂变换 → 验证 → 回滚** 完整闭环。

---

## 阶段二：多 Agent + 受控生成（Week 5–10）

**目标**：从一个 Agent 单 workspace 走向多 Agent 协作。

### Week 5–6：Workspace 分层隔离

| # | 任务 | 工时 | 说明 |
|:-:|:-----|:----:|:-----|
| 🟡 P1 | `workspace:create-child` | 2d | 子 workspace（孤立实验） |
| 🟡 P1 | `workspace:isolate`（锁/权限） | 2d | 只读/只写保护 |
| 🟡 P1 | `workspace:sync-from` / `workspace:merge` | 2–3d | workspace 间同步与合并 |

**里程碑：** 两个 Agent 可操作不同子 workspace，互不干扰。

### Week 7–8：Inter-Agent Messaging

| # | 任务 | 工时 | 说明 |
|:-:|:-----|:----:|:-----|
| 🟡 P1 | `(send 'agent-b message)` + mailbox polling | 3–5d | 轻量 actor 模型，基于 session/serve |

**里程碑：** Agent A 发消息, Agent B 收到并回复。

### Week 9–10：合成策略 + 规则系统

| # | 任务 | 工时 | 说明 |
|:-:|:-----|:----:|:-----|
| 🟡 P1 | `synthesize:define` 多策略后端 | 3d | LLM / genetic / template 策略 |
| 🟢 P2 | `rule:normalize` 系统 | 4–5d | Agent 学规范、存规范、应用规范 |

**里程碑：** Agent 能自动保持代码库风格一致。

**阶段二验收：** 两个 Agent 分别维护不同模块，可互相发消息协调，代码变更遵从规范化规则。

---

## 阶段三：可靠性与工业级（Week 11–16）

**目标**：安全、可观测、性能，让 Aura EDSL 能上真实项目。

### Week 11–12：Agent 沙箱 + 资源限制

| # | 任务 | 工时 | 说明 |
|:-:|:-----|:----:|:-----|
| 🟢 P2 | 权限模型（module / symbol whitelist） | 3d | mutation 只允许特定 scope |
| 🟢 P2 | eval 资源限制（CPU / memory / recursion depth） | 3–4d | serve 模式可安全暴露给外部 Agent |

**里程碑：** serve 模式可安全暴露给不可信 Agent。

### Week 13–14：调试 & 可观测性深化

| # | 任务 | 工时 | 说明 |
|:-:|:-----|:----:|:-----|
| 🟢 P2 | `ast:explain-diff` — 自然语言解释变更 | 2d | 结构化 diff → 文本描述 |
| 🟢 P2 | `compile:timeline` — 编译事件日志 | 2d | 每次增量编译的完整审计轨迹 |
| 🟢 P2 | 交互式 AST browser（REPL mode） | 2–3d | 树形浏览 / 搜索 / 标注 |

### Week 15–16：性能 & 生态

| # | 任务 | 工时 | 说明 |
|:-:|:-----|:----:|:-----|
| 🟢 P2 | 并行查询 / 增量 typecheck | 3–5d | 不阻塞 eval |
| ⚪ P3 | VS Code / Cursor 插件 | 5–10d | 原生 EDSL 查、改 AST |

**阶段三验收：** Agent EDSL 操作有完备的可审计日志、沙箱隔离、可视化 AST 浏览。

---

## 阶段四：前瞻（Q3–Q4 2026）

| # | 任务 | 说明 |
|:-:|:-----|:------|
| ⚪ P3 | 遗传算法变异策略 | 替代 LLM intend 的备选生成方案 |
| ⚪ P3 | 分布式 EDSL | 多机共享 AST workspace |
| ⚪ P3 | 形式化证明接口 | 关键 mutate 走 proof 而非 test |
| ⚪ P3 | LangGraph / CrewAI 集成 | 适配主流 Agent 框架 |

**周期量级：** 每月 1–2 项，不设硬 deadline。

---

## 一览图

```
Week  1–2 │ query:semantic + snapshot/diff + summary     │ 🔴 P0
Week  3–4 │ visualize + mutate:insert/splice/refactor    │ 🔴 P0 / 🟡 P1
─── ⛳ Agent 自我闭环 ───
Week  5–6 │ workspace 分层 + 权限 (P0 + P1 COW + lock complete)   │ 🟡 P1
Week  7–8 │ inter-agent messaging (pending)                        │ 🟡 P1
Week  9–10│ synthesize:define + rule:normalize             │ 🟡 P1 / 🟢 P2
─── ⛳ 多 Agent 协作 ───
Week 11–12│ 沙箱 + 资源限制                                │ 🟢 P2
Week 13–14│ 调试 & 可观测性                                │ 🟢 P2
Week 15–16│ 性能优化 + IDE 插件                            │ 🟢 P2 / ⚪ P3
─── ⛳ 生产可用 ───
Q3–Q4     │ 进化策略 / 分布式 / 证明 / 框架集成            │ ⚪ P3
```
