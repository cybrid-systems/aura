# Aura 文档

## 概况

| 文档 | 说明 |
|------|------|
| [教程](tutorial.md) | 上手实践，从零开始 |
| [API 参考](api-reference.md) | 所有原语参考 |
| [路线图](roadmap.md) | 发展方向 |
| [基准](benchmark.md) | EDSL benchmark 数据 |
| [哲学](philosophy.md) | Aura 的设计思想 |

## 设计文档 (`design/`)

设计文档分三层：

- **`design/core/`** — 高价值核心设计 (~6 篇) ，作为新贡献者的 on-ramp
- **`design/compilation/`** — 编译相关 (IR / JIT) (~2 篇)
- **`design/runtime/`** — 运行时相关 (async serve / FFI) (~2 篇)
- **`design/notes/`** — 归档的设计探索 / 单一议题 follow-up (~80+ 篇) — 仍有参考价值但不是 on-ramp 必读

### 核心 (`design/core/`)

| 文档 | 内容 |
|------|------|
| [agent_orchestration](design/core/agent_orchestration.md) | Agent 编排模型、意图、inter-agent 通信 |
| [mutate_api](design/core/mutate_api.md) | 动态变更 / 自演化 API |
| [typesystem](design/core/typesystem.md) | 类型系统设计 + 形式化 |
| [query_edsl](design/core/query_edsl.md) | 查询 EDSL（只读 AST 访问） |
| [typed_mutation](design/core/typed_mutation.md) | 类型化变更的形式 |
| [workspace_layering](design/core/workspace_layering.md) | Workspace 分层 / COW / lock |

### 编译 (`design/compilation/`)

| 文档 | 内容 |
|------|------|
| [ir_pipeline](design/compilation/ir_pipeline.md) | IR 管线设计 |
| [jit](design/compilation/jit.md) | LLVM ORC JIT 设计 |

### 运行时 (`design/runtime/`)

| 文档 | 内容 |
|------|------|
| [async_serve](design/runtime/async_serve.md) | Fiber / scheduler / 多 session serve 模式 |
| [ffi](design/runtime/ffi.md) | C FFI 设计与绑定 |

### 归档 (`design/notes/`) — 按需查阅

包含所有 issue follow-up 、speculative research、单一议题的设计探索、之前版本的管线设计等 (~80+ 篇)。新贡献者可以**跳过**这个目录；它主要供历史参考。`git log -- docs/design/notes/<file>` 仍可追溯每个文件的来历。

## 构建

构建指南见[项目 README](../README.md#构建)。
