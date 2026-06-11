# Aura 文档

> **推荐阅读路径**
> - **AI Agent / LLM 写手**： [tutorial.md](tutorial.md) → **[api-reference.md](api-reference.md)**（Primitives Surface + 代码位置） → `design/core/` 的 **## 0. Implementation Status** 表格。
> - **理解自修改核心**： `design/core/query_edsl.md` + `mutate_api.md` + `workspace_layering.md` + `typed_mutation.md`。
> - **Agent 编排**： `design/core/agent_orchestration.md` + `std/orchestrator.aura` + `projects/evo-kv/`。
> - **贡献核心**： `docs/developer/evaluator.md` + `projects/GAPS.md`。

**文档分离**：`tutorial.md` + `api-reference.md` 作为 User Guide/Surface；`design/core/` 等作为 Design & Extension。

## 概况

| 文档 | 说明 |
|------|------|
| [教程](tutorial.md) | 上手 + 当前实装状态 |
| [API 参考](api-reference.md) | 原语 + 代码/设计交叉引用 |
| [路线图](roadmap.md) | 项目驱动迭代现状 |
| [基准](benchmark.md) | EDSL benchmark 数据 |
| [哲学](philosophy.md) | 设计思想 |

**项目驱动**：主要通过 `projects/evo-kv/` 等真实项目暴露 gap 并修复核心。见 [projects/README.md](../projects/README.md) 和 `projects/GAPS.md`。

## 设计文档 (`design/`)

分四层：

- **`design/core/`**（7 篇） — 新贡献者 on-ramp，高价值活文档
- **`design/compilation/`**（2 篇） — IR / JIT
- **`design/runtime/`**（2 篇） — async serve / FFI
- **`design/history/`** — 归档（`notes/` + `closings/`）。新人可跳过，详见各子目录 README。

### Living Documentation Practices（活文档维护）

- 所有 `core/`、`compilation/`、`runtime/` 文档必须有 `## 0. Implementation Status` 章节（C++ Core Layer + Aura Layer 两表 + 日期 + AI Agent 注意事项）。
- 新增/修改 primitive 时必须同步更新：`api-reference.md` + 对应 core/ §0 表 + 必要时 `tutorial.md`。
- 归档内容只放 speculative 探索；实际工作走 core/ 更新 + closing。
- 状态横幅与日期必须保持新鲜。

参考实现见各 core/ 文档。详细背景见 Issue #156 相关 closing。

### 核心 (`design/core/`)

| 文档 | 内容 |
|------|------|
| [agent_orchestration](design/core/agent_orchestration.md) | Agent 编排、意图、inter-agent 通信 |
| [mutate_api](design/core/mutate_api.md) | 结构化变异 / 自演化 API |
| [typesystem](design/core/typesystem.md) | 类型系统 + 形式化 |
| [query_edsl](design/core/query_edsl.md) | 查询 EDSL |
| [typed_mutation](design/core/typed_mutation.md) | 类型化变更 |
| [workspace_layering](design/core/workspace_layering.md) | Workspace 分层 / COW / lock |
| [memory_model](design/core/memory_model.md) | 内存模型 + 锁协议（#157 Phase 4） |

### 编译 (`design/compilation/`)

| 文档 | 内容 |
|------|------|
| [ir_pipeline](design/compilation/ir_pipeline.md) | IR 管线 |
| [jit](design/compilation/jit.md) | LLVM ORC JIT |

### 运行时 (`design/runtime/`)

| 文档 | 内容 |
|------|------|
| [async_serve](design/runtime/async_serve.md) | Fiber / scheduler / 多 session |
| [ffi](design/runtime/ffi.md) | C FFI |

### 归档与历史 (`design/history/`) — 按需查阅

- `design/history/notes/`：归档设计探索、issue follow-up（~80+ 篇）。新人跳过。
- `design/history/closings/`：issue 关闭总结。已统一归档。

新贡献者优先读 `design/core/` 的 Implementation Status。历史文档用 `git log` 追溯。

**注意**：所有核心设计文档必须包含 `## 0. Implementation Status` 章节（C++ / Aura 两表 + AI Agent 注意事项）。

## 构建

构建指南见 [项目 README](../README.md#构建)。
