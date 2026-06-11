# Aura 路线图

## 现有能力

Aura 是一个自修改 Lisp 运行时，核心能力包括：

- 完整的 Lisp 求值器（树遍历 + IR 双路径）与 Sound Gradual Typing 类型系统（occurrence typing、Let-Poly、ADT 穷尽性单层、线性所有权 M4）。
- 自修改 EDSL（`query:*` 11+ 原语 + `mutate:*` 12+ 原语 + `ast:*` snapshot/restore/rollback + `workspace:*` P0 COW 分层）让 AI Agent 在运行时**精确读写和版本化管理自身代码**。
- 原子 mutate + rollback + panic checkpoint + typed mutation + `mutate:query-and-replace`。
- Agent 编排层：`std/orchestrator` + `std/agent` 提供 `orch:conduct` / `orch:pipeline` / `orch:parallel`（真并行）等；fiber scheduler（多线程 + work-stealing）+ mailbox + 跨 session 通信。
- LLVM ORC JIT 后端（Phase 1-5 全部完成：38 opcode、闭包、prim bridge、增量 cache、hot-swap、O2；~7.55× 加速）；AOT 路径处于设计阶段。
- 标准库覆盖 50+ 模块（含 query/refactor/workspace、evolve、orchestrator、llm、synthesize 等）。
- `--serve` / `--serve-async` JSON 协议（支持 session、typed-mutate、mutation-log、rollback 等），适合 LLM Agent 驱动的自演化循环。
- EDSL Benchmark：145+ 任务，Grok/MiniMax-M3 达到 83-85% 通过率。

项目驱动迭代正在进行：`projects/evo-kv/`（自演化 KV + AOF + pubsub + 自动修复）是当前最活跃的驱动项目，已暴露并修复多批 core gap。`projects/kv/` 和 `projects/chat/` 已达到 ✅ works 状态。

## 路线图

### 开发方法论

从 P2.8 开始，采用 **Project-Driven Iteration**（详见 `docs/design/projects_iteration.md`）：

```
写真实项目 → 暴露 core gaps → 修复核心 → 写更难的项目
```

每写一个 `projects/` 下的 demo，至少暴露 3-5 个 Aura 核心短板。修复这些短板是 P 系列迭代的主要驱动力。

### 当前活跃项目（Project-Driven 迭代现状）

| 项目 | 状态 | 说明 |
|------|------|------|
| [evo-kv/](projects/evo-kv/) | 🟡 主要驱动 | 自演化 KV（core + evolve + auto + grok + metrics + pubsub + AOF + zset）。持续暴露并修复 mutate、workspace、concurrency、AOT tiering 等 gap。 |
| [kv/](projects/kv/) | ✅ works | 最小键值存储，已完成多批 core gap 修复。 |
| [chat/](projects/chat/) | ✅ works | session 间消息，已内建 send/recv/broadcast。 |
| cli / todo / calc-plugin 等 | ⬜ planning | 规划中，待 evo-kv 驱动更多 gap 后启动。 |

所有已发现的 core gap 跟踪在 `projects/GAPS.md`。

### P 系列核心规划（已更新至 2026-06）

| 阶段 | 内容 | 现状 |
|------|------|------|
| **P2.7b** | 原语作为闭包值传递 AOT 支持 | ✅ 已完成 |
| **P2.8** | Project-Driven 起步 + `projects/` 目录 | ✅ 完成（kv + chat + evo-kv 已落地） |
| **P3** | 类型系统补全 + 增量热更新 + Versioned Types | 🟡 进行中（typesystem §0 显示 Let-Poly/ADT 单层/增量已实装，IR 类型流入仍有限；evo-kv 持续驱动） |
| **P4** | 标准库补全 + 高级 refactor helper | 🟡 进行中（std/ 已 50+ 模块，refactor/workspace helper 仍较薄） |
| **P5** | 运行时沙箱、Capability Effects、跨模块检查 | 🔴 设计中（与 typed mutation / evolve 相关） |
| **P6** | Serve 协议成熟 + 网络层 / 持久化 agent 状态 | 🟡 进行中（--serve-async + fiber scheduler 已强，跨 host / 持久化是 Phase 4 项） |

## 每个 project 的预期产出

1. `projects/<name>/` 下的可运行代码
2. 修复的 core gap 列表（GAPS.md 中 tracking）
3. 新增/改进的 core API（commit to main）
4. 对应 core 功能的测试用例

## 如何参与

1. 选一个 project（建议从 P1 开始）
2. 写代码 → 跑 → 记录 gap
3. 修 core gap → 提 PR
4. 项目完成后更新状态
5. 选下一个 project
