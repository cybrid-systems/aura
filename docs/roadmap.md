# Aura 路线图

**更新：2026-05-25 (EDSL + Agent 交互能力全部落地)**

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

## 🚧 剩余工作 (Phase 3 — 生产可用)

### Agent 沙箱 + 资源限制

| 优先级 | 任务 | 工时 | 说明 |
|:------:|:-----|:----:|:-----|
| 🟢 P2 | 权限模型（module / symbol whitelist） | 3d | mutation 只允许特定 scope |
| 🟢 P2 | eval 资源限制（CPU / memory / recursion depth） | 3-4d | serve 模式安全沙箱 |

### 调试 & 可观测性

| 优先级 | 任务 | 工时 | 说明 |
|:------:|:-----|:----:|:-----|
| 🟢 P2 | `ast:explain-diff` — 自然语言解释变更 | 2d | 结构化 diff → 文本 |
| 🟢 P2 | `compile:timeline` — 编译事件日志 | 2d | 增量编译审计轨迹 |
| 🟢 P2 | 交互式 AST browser (REPL mode) | 2-3d | 树形浏览 |

### 性能 & 生态

| 优先级 | 任务 | 工时 | 说明 |
|:------:|:-----|:----:|:-----|
| 🟢 P2 | 并行查询 / 增量 typecheck | 3-5d | 不阻塞 eval |
| ⚪ P3 | VS Code / Cursor 插件 | 5-10d | 原生 EDSL 支持 |
| 🟢 | struct 模块 AOT | — | `define-type`(EDSL)，IR 路径 |
| 🟢 | LSP / 包管理 / 自举 | — | 独立长期项目 |
| 🟢 | 性能优化 (O3/LTO) | — | 功能完整后再做 |

---

## 🔭 前瞻 (Q3-Q4 2026)

| 任务 | 说明 |
|:-----|:------|
| 分布式 EDSL | 多机共享 AST workspace |
| 形式化证明接口 | 关键 mutate 走 proof 而非 test |
| LangGraph / CrewAI 集成 | 适配主流 Agent 框架 |
