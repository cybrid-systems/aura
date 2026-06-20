# 架构概览

代码为真相。本文是模块地图；细节见 `src/` 与 `tests/suite/`。

## 数据流

```
源码 ──► parser ──► FlatAST (workspace)
              │
              ▼
         type_checker ──► 类型 / 约束
              │
              ▼
         lowering ──► IRModule
              │
      ┌───────┴───────┐
      ▼               ▼
 eval_flat       ir_executor / aura_jit
 (树遍历)         (解释 / LLVM ORC)
      │               │
      └───────┬───────┘
              ▼
         EvalValue 结果
```

Agent 自修改路径：`set-code` → `query:*` / `mutate:*` → `eval-current`（可走 JIT）。JSON 入口：`--serve-async`（见 [wire-formats.md](wire-formats.md)）。

## C++ 模块（`export module`）

| 模块 | 路径 | 职责 |
|------|------|------|
| `aura.parser.*` | `src/parser/` | 词法、语法 → FlatAST |
| `aura.core.*` | `src/core/` | AST 节点、Arena、类型基础设施 |
| `aura.compiler.evaluator` | `evaluator.*` | 运行时中枢、原语、eval_flat |
| `aura.compiler.query` | `query.*` | QueryEngine、DefUseIndex |
| `aura.compiler.type_checker` | `type_checker.*` | 渐进类型、Let-Poly、线性 |
| `aura.compiler.lowering` | `lowering.*` | AST → IR |
| `aura.compiler.ir` | `ir.ixx` | IR 指令、模块结构 |
| `aura.compiler.ir_executor` | `ir_executor.*` | IR 解释器 |
| `aura.compiler.service` | `service.ixx` | 编译服务、增量 cache、脏标记 |
| `aura.compiler.cache` | `cache.*` | EDSL V2 source-hash cache |
| JIT | `aura_jit.*` | LLVM ORC、hot-swap、prim bridge |
| Serve | `src/serve/` | fiber、scheduler、mailbox、GC 协调 |

## 自修改 EDSL（C++ 原语）

在 `evaluator_impl.cpp` 经 `primitives_.add` 注册（约 400+ 个）。核心集群：

| 集群 | 示例 | 测试 |
|------|------|------|
| 加载/执行 | `set-code`, `eval-current` | `tests/suite/core.aura` |
| Query | `query:find`, `query:pattern`, `query:where` | `tests/suite/edsl_errors.aura` |
| Mutate | `mutate:rebind`, `mutate:query-and-replace` | `tests/suite/mutate-structured.aura` |
| 版本 | `ast:snapshot`, `ast:restore`, `rollback` | `tests/panic_rollback.aura` |
| Workspace | `workspace:create`, `workspace:merge`, `workspace:lock` | `tests/suite/module.aura` |

Aura 层 helper：`lib/std/query.aura`（3 个）、`lib/std/refactor.aura`、`lib/std/workspace.aura`。

## Agent 编排

- C++：`fiber:*`, `send`/`recv`, `session:*`（`src/serve/`）
- Aura：`lib/std/orchestrator.aura`（`orch:*`, `agent:*`）
- 测试：`tests/suite/orchestrator.aura`

## 标准库

`lib/std/*.aura` — 每个文件 `(export …)` + 文件头注释即 API。加载：`(require "std/list" all:)`。

## 贡献运行时

读 [contributing.md](contributing.md)（FlatAST 不变式、锁、defuse）。历史设计文档在 `git tag docs-archive-pre-2026-06`。