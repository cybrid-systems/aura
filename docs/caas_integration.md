# ASTArena + Compiler as a Service 集成方案

## 架构总览

```
┌─────────────────────────────────────────────────────┐
│                  CompilerService                     │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────┐  │
│  │   ASTArena    │  │   Parser     │  │ Evaluator │  │
│  │  (pmr bump)   │──┤  (arena ref) │  │ (env +    │  │
│  │  8MB backing  │  │              │  │  closure) │  │
│  └──────┬───────┘  └──────────────┘  └─────┬─────┘  │
│         │                                  │         │
│         ▼                                  ▼         │
│  ┌────────────────────────────────────────────────┐  │
│  │            IR Pipeline (optional)               │  │
│  │  LoweringPass → ComputeKind → Arity → Interp   │  │
│  └────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
         │
         │ 每次请求：reset() → parse → eval → 返回结果
         ▼
    main.cpp / REPL / ABF pipe / Compiler-as-a-Service
```

## 组件职责

### ASTArena（pmr 内存池）

```cpp
// src/core/arena.ixx
class ASTArena {
    std::pmr::monotonic_buffer_resource resource_;  // bump alloc
    std::vector<std::byte> buffer_;                 // 8MB backing
    std::size_t bytes_allocated_;                   // 调试统计

    template<typename T> T* create(args...);   // allocate + construct_at
    void reset();                               // release all, O(1)
};
```

**关键设计决策：**
- 使用 `monotonic_buffer_resource`+ 自有 vector backing（无需依赖外部 allocator）
- `null_memory_resource()` 为上游——不 fallback 到堆分配（避免碎片）
- `reset()` = `release()` + 归零计数器，不释放 buffer（所以后续 `create()` 仍然可用）
- 每个 `CompilerService` 实例持有一个 `ASTArena`，编译请求间复用

### CompilerService（会话生命周期）

```cpp
// src/compiler/service.ixx
class CompilerService {
    ASTArena arena_;
    Parser parser_;     // 引用 arena_
    Evaluator evaluator_;  // 引用 arena_

    void reset();                 // 重置 arena（parser/evaluator 引用不变）
    EvalResult eval(input);       // 树遍历模式
    EvalResult eval_ir(input);    // IR 管线模式
};
```

**每次请求的典型生命周期：**

```
1. cs.reset()            — 释放上次 AST 的全部内存（O(1)）
2. cs.eval("(+ 1 2)")    — 在空 arena 上分配 AST → 求值
3. 返回结果              — arena 保留，等待下次请求
```

对于 REPL 场景：循环 `reset → eval → 输出`，arena 自动复用。

## 多模块 / 增量编译模式

```
┌─────────────────────────────────────────────┐
│           CompilerService Manager            │
│                                              │
│  ┌────────┐  ┌────────┐  ┌────────┐         │
│  │ Module  │  │ Module │  │ Module │  ...    │
│  │ Arena 1 │  │ Arena 2│  │ Arena 3│         │
│  └────────┘  └────────┘  └────────┘         │
│                                              │
│  ModuleA.reset()   // 单独释放 module A      │
│  ModuleB.create()  // module B 继续使用      │
└─────────────────────────────────────────────┘
```

每个模块独立 arena，支持：
- 细粒度释放：只 reset 改动的模块
- 并行编译：多 arena 安全
- 持久化符号表：用 `std::pmr::vector` 从 arena 外部保留关键数据

## AI Agent 调用场景

```cpp
// Compiler as a Service — 持久进程 + pipe/UDS
CompilerService cs;

while (true) {
    auto request = read_request(stdin);  // 从 Agent 收到编译请求
    cs.reset();
    auto result = cs.eval_ir(request.code);
    auto diags = arity_checker.check(last_module);
    write_response(stdout, {result, diags});
    // arena 已 reset，准备下一请求
}
```

## 内存对比

| 指标 | 旧实现（vector bump） | 新实现（pmr monotonic） |
|------|----------------------|------------------------|
| 分配速度 | 指针偏移 | 指针偏移（同等）|
| 对齐处理 | 手动 `(pos + align - 1) & ~(align - 1)` | 自动（传给 `allocate`） |
| pmr 容器 | 不支持 | 原生支持 `pmr::vector/string/map` |
| 释放 | 指针重置 | `release()` 等效 |
| 安全性 | 直接 `construct_at` | 等效 |
| 初始大小 | 64KB | 8MB（pmr 推荐） |

## 文件变更清单

```
NEW: src/compiler/service.ixx         — CompilerService
MOD: src/core/arena.ixx               — pmr-based arena
MOD: src/main.cpp                      — 使用 CompilerService
NEW: docs/caas_integration.md          — 本文档
```

## 后续步骤

1. CompilerService 增加 `--serve` 模式（UDS pipe 长连接）
2. ABF deserializer 集成到 CompilerService（`eval_abf()`）
3. 多 module arena 管理器
4. Pass Manager 注册到 CompilerService
