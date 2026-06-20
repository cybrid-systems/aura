# Aura 路线图

## 现有能力

Aura 是一个自修改 Lisp 运行时，核心能力包括：

- 完整的 Lisp 求值器（树遍历 + IR 双路径）与 Sound Gradual Typing 类型系统（occurrence typing、Let-Poly、ADT 穷尽性单层、线性所有权 M4）。
- 自修改 EDSL（`query:*` 11+ 原语 + `mutate:*` 12+ 原语 + `ast:*` snapshot/restore/rollback + `workspace:*` P0 COW 分层）让 AI Agent 在运行时**精确读写和版本化管理自身代码**。
- 原子 mutate + rollback + panic checkpoint + typed mutation + `mutate:query-and-replace`。
- Agent 编排层：`std/orchestrator` + `std/agent` 提供 `orch:conduct` / `orch:pipeline` / `orch:parallel`（真并行）等；fiber scheduler（多线程 + work-stealing）+ mailbox + 跨 session 通信。
- LLVM ORC JIT 后端（Phase 1-5 全部完成：38 opcode、闭包、prim bridge、增量 cache、hot-swap、O2）；AOT 路径处于设计阶段。
- 标准库覆盖 50+ 模块（含 query/refactor/workspace、evolve、orchestrator、llm、synthesize 等）。
- `--serve` / `--serve-async` JSON 协议（支持 session、typed-mutate、mutation-log、rollback 等），适合 LLM Agent 驱动的自演化循环。
- EDSL Benchmark：145+ 任务，Grok/MiniMax-M3 达到 83-85% 通过率。

## 路线图

### 开发方法论

采用 **Test-Driven / Example-Driven Iteration**（原 project-driven 迭代；`projects/` 目录已移除，见 commit `2882e37`）：

```
写真实用例（tests/suite、tests/regression、lib/std demo）→ 暴露 core gaps → 修复核心 → 写更难用例
```

每个端到端用例应能暴露 3-5 个 Aura 核心短板；修复这些短板是 P 系列迭代的主要驱动力。早期方法论见 git tag `docs-archive-pre-2026-06` 下的 `docs/design/notes/projects_iteration.md`。

### 当前验证场（替代原 projects/）

| 用例 / 模块 | 说明 |
|-------------|------|
| `tests/suite/orchestrator.aura` | Agent 编排与并行管线 |
| `tests/suite/mutate-structured.aura` | 结构化 mutate + rollback |
| `tests/suite/edsl_errors.aura` | query/mutate 错误路径 |
| `tests/test_serve_async.aura` | serve-async JSON 协议 |
| `lib/std/orchestrator.aura` | 高层 `orch:*` / `agent:*` |
| `lib/std/evolve.aura` | 自演化辅助 |

缺口与后续工作跟踪在 **GitHub Issues**，不再维护单独的 `projects/GAPS.md`。

### P 系列核心规划（2026-06）

| 阶段 | 内容 | 现状 |
|------|------|------|
| **P2.7b** | 原语作为闭包值传递 AOT 支持 | ✅ 已完成 |
| **P2.8** | 示例驱动迭代 + 端到端用例 | ✅ 完成（`tests/suite/` + `lib/std/` 覆盖） |
| **P3** | 类型系统补全 + 增量热更新 + Versioned Types | 🟡 进行中（Let-Poly/ADT 单层/增量已实装；IR 类型流入仍有限） |
| **P4** | 标准库补全 + 高级 refactor helper | 🟡 进行中（std/ 已 50+ 模块，refactor/workspace helper 仍较薄） |
| **P5** | 运行时沙箱、Capability Effects、跨模块检查 | 🔴 设计中（与 typed mutation / evolve 相关） |
| **P6** | Serve 协议成熟 + 网络层 / 持久化 agent 状态 | 🟡 进行中（--serve-async + fiber scheduler 已强，跨 host / 持久化是 Phase 4 项） |

## 每个迭代的预期产出

1. `tests/suite/` 或 `tests/regression/` 下的可运行用例
2. 修复的 core gap（对应 GitHub issue）
3. 新增/改进的 core API（commit to main）
4. 对应 core 功能的 C++ 单元测试（`test_ir` 等）

## 如何参与

1. 从 `tests/suite/` 或 `lib/std/` 选一个场景延伸
2. 写用例 → `./build.py check` → 记录 gap（开 issue）
3. 修 core gap → 提 PR
4. 用例通过后更新 `roadmap.md` 或关闭 issue