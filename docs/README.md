# Aura 文档

> **真相层级**：代码 + 测试 > 运行时 `(api-reference)` > 本目录手写文档 > `git log` 历史。

## 我想…

| 目标 | 去看 |
|------|------|
| 快速上手 | [tutorial.md](tutorial.md) |
| 有哪些原语 | 运行 `(api-reference)`，或读 [api-reference.md](api-reference.md)（过渡；后续 codegen） |
| Agent 走 JSON 协议 | [wire-formats.md](wire-formats.md) + `tests/test_serve_async.aura` |
| 自修改 EDSL 示例 | `tests/suite/mutate-structured.aura`、`tests/suite/edsl_errors.aura` |
| Agent 编排 | `lib/std/orchestrator.aura` + `tests/suite/orchestrator.aura` |
| 标准库模块 | `lib/std/*.aura`（文件头注释 + `(export …)`） |
| 模块 / 管线结构 | `src/compiler/`、`src/serve/`；设计概要见 `design/core/`、`design/compilation/` |
| 改 evaluator / 加 primitive | [developer/evaluator.md](developer/evaluator.md) |
| 构建与测试 | `./build.py check`（见仓库根 [README.md](../README.md)） |
| 方向与里程碑 | [roadmap.md](roadmap.md) + GitHub Issues |
| 历史设计 / issue 结案 | `design/history/`（归档，非当前真相） |

## 用户指南

| 文档 | 说明 |
|------|------|
| [tutorial.md](tutorial.md) | 可运行示例；特性以 `tests/suite/` 为准 |
| [api-reference.md](api-reference.md) | 原语速查（手写，逐步由代码生成替代） |
| [wire-formats.md](wire-formats.md) | `--serve` / `--serve-async` JSON 协议 |

## 开发者

| 文档 | 说明 |
|------|------|
| [developer/evaluator.md](developer/evaluator.md) | FlatAST 不变式、加 primitive、并发 footgun |
| [roadmap.md](roadmap.md) | P 系列规划与当前重点 |
| [benchmark.md](benchmark.md) | EDSL benchmark 数据（源：`tests/benchmark.py`） |

## 设计文档（`design/`）

**注意**：`design/core/` 等文档可能落后于代码。以测试、`(api-reference)` 和 `src/` 为准。

- **`design/core/`** — 自修改、类型、workspace、编排（8 篇）
- **`design/compilation/`** — IR 管线、JIT（2 篇）
- **`design/runtime/`** — async serve、FFI（2 篇）
- **`design/history/`** — 归档说明；历史正文见 git tag `docs-archive-pre-2026-06`

### 核心 (`design/core/`)

| 文档 | 内容 |
|------|------|
| [query_edsl](design/core/query_edsl.md) | 查询 EDSL |
| [mutate_api](design/core/mutate_api.md) | 结构化变异 |
| [typed_mutation](design/core/typed_mutation.md) | 类型化变更 |
| [workspace_layering](design/core/workspace_layering.md) | Workspace 分层 / COW |
| [agent_orchestration](design/core/agent_orchestration.md) | Agent 编排 |
| [typesystem](design/core/typesystem.md) | 类型系统 |
| [memory_model](design/core/memory_model.md) | 内存模型与锁 |

## 构建

```bash
./build.py build    # 或 cmake -B build && cmake --build build --target aura -j
./build.py check    # CI 默认：构建 + 核心测试
```