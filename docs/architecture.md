# 架构概览

代码为真相。本文件是模块地图。

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

Agent 自修改：`set-code` → `query:*` / `mutate:*` → `eval-current`(可走 JIT)。JSON 入口：`--serve-async`(见 [wire-formats.md](wire-formats.md))。

并行 Agent：`parallel-intend` / `orch:spawn-agent` → Scheduler fibers → `Fiber::join` + MultiFiberMailbox → 结果 hash。

## C++ 模块

| 模块 | 路径 | 职责 |
|------|------|------|
| `aura.parser.*` | `src/parser/` | 词法 / 语法 → FlatAST |
| `aura.core.*` | `src/core/` | AST / Arena / 类型基础设施 / ResourceQuota |
| `aura.compiler.evaluator` | `evaluator.ixx` + 44 `.cpp` | 运行时中枢 / 原语 / eval_flat |
| `aura.compiler.query` | `query.*` | QueryEngine / DefUseIndex |
| `aura.compiler.type_checker` | `type_checker.*` | 渐进类型 / Let-Poly / 线性 |
| `aura.compiler.lowering` | `lowering.*` | AST → IR |
| `aura.compiler.ir_executor` | `ir_executor.*` | IR 解释器 |
| `aura.compiler.service` | `service.ixx` | 编译服务 / 增量 cache / 脏标记 |
| JIT | `aura_jit.*` | LLVM ORC / hot-swap / prim bridge |
| Serve | `src/serve/` | fiber / scheduler / mailbox / parallel_orch / GC |
| Orch | `src/orch/` | unified agent_spawn / parallel_orch facade (#1588) |

## Evaluator 分区

`evaluator.ixx` 接口 + 44 实现 TU(同 module partition)。原语经
`evaluator_primitives_registry.cpp` 编排;构造器内原语在
`evaluator_ctor.cpp`。

| 层 | TU | 入口 |
|----|-----|------|
| 求值 | `evaluator_eval_flat.cpp` | `eval_flat` / `apply_closure` |
| 环境 | `evaluator_env.cpp` | `Env::lookup*` / `bind_symid` |
| 自修改 | `evaluator_workspace_tree.cpp` · `evaluator_fiber_mutation.cpp` | `workspace_mtx_` / `mutate:*` |
| 查询 | `evaluator_primitives_query*.cpp` · `evaluator_query_index.cpp` | `query:*` |
| 原语表 | `evaluator_primitives_registry.cpp` | `register_all_primitives` |

文件列表见 [contributing.md §文件地图](contributing.md#文件地图)。

## Agent 编排

| 层 | 路径 | 职责 |
|----|------|------|
| Fiber runtime | `src/serve/fiber.h` · `scheduler.h` | M:N 调度 / `Fiber::join` (#1584) |
| Mailbox | `src/serve/multi_fiber_mailbox.h` | multi-attach / 背压 (#1585) |
| Parallel batch | `src/serve/parallel_orch.h` | `parallel_intend` 并发 cap / timeout (#1586) |
| Composition | **`src/orch/`** | `agent_spawn` + mailbox + join (#1588) |
| Aura 表面 | `evaluator_primitives_agent.cpp` | `(parallel-intend)` / `orch:spawn-agent` |

跨 fiber 不共享裸 `NodeId`;mutation 必须 `MutationBoundaryGuard`。

## Primitive vs Stdlib 边界

Primitive 是 C++ 注册到 `Primitives` 表的 entry,Stdlib 是 `lib/std/*.aura` 里 `(export …)` 的 Aura 函数。**默认下沉决策:stdlib**。只有满足决策框架 7 条红线(engine-boot / 内部状态访问 / 性能热路径 / FFI / 类型系统 / 观测性 / 诊断恢复)才下沉为 primitive。注册点见 [contributing.md §2](contributing.md)。