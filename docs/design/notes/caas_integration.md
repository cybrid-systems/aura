# Compiler as a Service (CaaS) 集成方案

## 架构总览（2026-05-18 更新）

```
┌──────────────────────────────────────────────────────────────┐
│                     CompilerService                           │
│                                                               │
│  ┌──────────────────────────────────────────────┐            │
│  │               eval(input)                     │            │
│  │   ┌──────────┐   ┌────────────┐              │            │
│  │   │  parse    │──→│ macro-exp  │              │            │
│  │   │ (FlatAST) │   │  (prepass) │              │            │
│  │   └─────┬─────┘   └─────┬──────┘              │            │
│  │         │               │                     │            │
│  │         ▼               ▼                     │            │
│  │   ┌────────────────────────────────────┐      │            │
│  │   │ needs_tree_walker_fallback?         │      │            │
│  │   ├── yes → Evaluator.eval_flat() ─────►│      │            │
│  │   └── no  ──► IR Pipeline                │      │            │
│  │              ├─ TypeCheckWrap (L2)       │      │            │
│  │              ├─ (define?) → cache + eval │      │            │
│  │              ├─ lower_to_ir_with_cache   │      │            │
│  │              ├─ ComputeKindWrap          │      │            │
│  │              ├─ ArityWrap                │      │            │
│  │              ├─ ConstantFoldingWrap      │      │            │
│  │              └─ IRInterpreter.execute()─►│      │            │
│  └──────────────────────────────────────────┘      │            │
│                                                     │            │
│  ┌──────────────────────────────────────┐           │            │
│  │   ArenaGroup（多模块内存管理）        │           │            │
│  │   ├─ main_arena (REPL/默认)          │           │            │
│  │   ├─ module_arena("lib/std") ≝ 8MB   │           │            │
│  │   └─ module_arena("app")     ≝ 8MB   │           │            │
│  └──────────────────────────────────────┘           │            │
│                                                     │            │
│  ┌──────────────────────────────────────┐           │            │
│  │   ir_cache_（增量编译）               │           │            │
│  │   ├─ "map"   → [IRFunction*]         │           │            │
│  │   ├─ "foldl" → [IRFunction*]         │           │            │
│  │   ├─ dep_graph_ (BFS 传递闭包)       │           │            │
│  │   └─ function_sources_ (重编译源码)  │           │            │
│  └──────────────────────────────────────┘           │            │
└──────────────────────────────────────────────────────┘            │
         │
         │  --serve JSON protocol (exec/define/mutate/rollback/session)
         ▼
    main.cpp / REPL / Agent
```

## 组件职责

### ASTArena（pmr 内存池 + SmallObjectPool）

```cpp
// src/core/arena.ixx (v3: SmallObjectPool)
class ASTArena {
    // 3-tier small-object pool (16/32/64 bytes, 3MB)
    SmallObjectPool small_pool_;
    // Main pmr monotonic_buffer_resource (8MB backing)
    std::pmr::monotonic_buffer_resource resource_;
    // Allocation path:
    //   create<T>() → sizeof(T) ≤ 64 → SmallObjectPool
    //               → else            → pmr monotonic buffer
};
```

**关键设计决策：**
- `monotonic_buffer_resource` + 自有 vector backing（无需依赖外部 allocator）
- `null_memory_resource()` 为上游——不 fallback 到堆分配（避免碎片）
- SmallObjectPool 3 层大小类（16/32/64 bytes）覆盖 90%+ AST 节点
- `reset()` = 两级同时释放，O(1)

### CompilerService（会话生命周期）

```cpp
// src/compiler/service.ixx
class CompilerService {
    ast::ASTArena arena_;           // 主 arena（REPL/单请求）
    ast::ArenaGroup arena_group_;   // 多模块 arena 管理器
    Evaluator evaluator_;

    void reset();                    // 重置主 arena
    EvalResult eval(input);          // IR-first + fallback（统一入口）
    EvalResult eval_ir(input);       // 纯 IR 管线（含 Pass Manager debug 输出）
    std::string typecheck(input);    // L6 渐进类型检查

    // 多模块
    ast::ASTArena& module_arena(const std::string& name);
    void reset_module(const std::string& name);

    // 增量编译
    EvalResult define_function(code);  // 定义 + 缓存 IR
    EvalResult exec_with_cache(code);  // 带缓存执行
    bool has_cached_function(name);
    std::size_t cached_function_count();

    // Mutation（EDSL）
    void set_code(input);           // 加载持久 AST
    MutationResult typed_mutate(code); // EDSL 变异
    std::vector<MutationLogEntry> query_mutation_log(node);
};
```

**每次请求的典型生命周期：**

```
1. cs.reset()            — 释放上次 AST 的全部内存（O(1)）
2. cs.eval("(+ 1 2)")    — 空 arena 上分配 AST → 求值
3. 返回结果              — arena 保留，等待下次请求
```

## 多模块 / 增量编译模式

```
┌─────────────────────────────────────────────┐
│           ArenaGroup                        │
│                                              │
│  ┌────────┐  ┌────────┐  ┌────────┐         │
│  │ Module  │  │ Module │  │ Module │  ...    │
│  │ Arena 1 │  │ Arena 2│  │ Arena 3│         │
│  └────────┘  └────────┘  └────────┘         │
│                                              │
│  ArenaGroup::reset_module("core")            │
│  ArenaGroup::module_arena("app")             │
└─────────────────────────────────────────────┘
```

每个模块独立 arena，支持：
- 细粒度释放：只 reset 改动的模块
- 模块级 compile/unload
- Per-module 内存统计

## IR 管线（默认路径）

```
eval(input)
  │
  ├─ parse → FlatAST
  ├─ macro_expand_all (prepass)
  ├─ needs_tree_walker_fallback?
  │   (EDSL / import / special form / unknown var)
  │
  ├── yes ──► Evaluator.eval_flat()
  │
  └── no  ──► TypeCheckWrap (Level 2, non-fatal)
               ├─ (define …) ──► cache_define() → IR cache → void
               └─ expression ──► lower_to_ir_with_cache()
                                  ├─ ComputeKindWrap
                                  ├─ ArityWrap
                                  ├─ ConstantFoldingWrap
                                  └─ IRInterpreter.execute()
```

### Fallback 情况

| 触发条件 | 原因 |
|----------|------|
| `import`/`use`/`require` | 环境绑定副作用（IR 无法复制） |
| EDSL `query:`/`mutate:` | 需要求值器内部状态 |
| `try`/`catch`/`when`/`unless` | 没有对应 IR 指令 |
| 未缓存的变量引用 | 运行时模块导入 |

## 增量编译

### 函数级缓存

```
cache_define(source, flat, pool, root, name)
  │
  ├─ TypeCheckWrap (Level 2, warning only)
  ├─ lower_to_ir_with_cache(flat, pool, arena, ir_cache, &hits)
  │   └─ 命中缓存函数时：inline 已编译的 IR，不重复 lowering
  ├─ per-function pass: ComputeKind + ConstantFold
  ├─ ir_cache_[name] = bundle of IRFunction[]
  ├─ ir_cache_bridge_[name] = ClosureBridgeData[]
  ├─ function_sources_[name] = raw source
  └─ record_dependency(name, each hit)
```

### 依赖追踪

```
dep_graph_:
  "foldl"  → { calls: [],            called_by: ["sum", "mean"] }
  "sum"    → { calls: ["foldl"],     called_by: ["stats"] }
  "mean"   → { calls: ["foldl"],     called_by: ["stats"] }
  "stats"  → { calls: ["sum","mean"], called_by: [] }

redefine("foldl"):
  BFS: foldl → sum → mean → stats
  → re-lower: sum, mean, stats (with updated foldl cache)
```

## --serve JSON 协议

常驻进程，每行一个 JSON 请求/响应，AI Agent 通过 stdin/stdout 交互。

### 支持的命令

| 命令 | 参数 | 说明 |
|------|------|------|
| `exec` | `code` | 计算表达式，返回结果 |
| `define` | `code` | 定义函数（缓存 IR），返回 |
| `mutate` | `op`, `node`, `value` | EDSL 类型变异 |
| `rollback` | `id` | 撤销变异 |
| `mutation-log` | `node` | 查询变异历史 |
| `session` | `name` | 多会话切换/创建 |
| `module` | `action`, `name`, `code` | 模块管理 (compile/unload/reload/list/stats) |

### 示例

```
→ {"cmd":"define","code":"(define (foldl f acc lst) ...)"}
← {"status":"ok","result":"#<void>"}

→ {"cmd":"exec","code":"(foldl + 0 (list 1 2 3))"}
← {"status":"ok","result":"6"}
```

## 实现状态（2026-05-23）

| 组件 | 状态 | 位置 | 备注 |
|------|------|------|------|
| ASTArena (pmr + SmallObjectPool) | ✅ v3 | `arena.ixx` | 16/32/64 三级，3MB 小对象池 |
| CompilerService | ✅ v2 | `service.ixx` | eval/define/mutate/typecheck |
| eval() IR-first + fallback | ✅ | `service.ixx` | 统一入口，自动降级 |
| eval_ir() 含 Pass Manager | ✅ | `service.ixx` | 纯 IR 管线 + debug 输出 |
| --serve JSON 协议 | ✅ v2 | `main.cpp` | exec/define/mutate/rollback/session |
| 多会话 (multi-session) | ✅ | `main.cpp` | session创建/切换 |
| ArenaGroup 基础设施 | ✅ v1 | `arena.ixx` | get-or-create / reset / stats |
| ArenaGroup 集成到 Service | ✅ v1 | `service.ixx` | compile_module + unload_module + reload_module |
| 增量编译 (函数级) | ✅ v1 | `service.ixx` | cache_define + dep_graph + invalidate |
| 增量编译 (模块级) | ✅ v1 | `service.ixx` | ModuleState dirty 追踪 + reload |
| 磁盘缓存 (mmap) | ✅ | `cache.ixx` + `cache_reflect.cpp` | CacheHeader 反射序列化, auto_validate |
| Level 2 类型检查 | ✅ | `pass_manager.ixx` | TypeCheckWrap pass (non-fatal warnings) |
| 函数热替换 + 依赖追踪 | ✅ | `service.ixx` | invalidate_function BFS re-lower |
| EDSL mutation | ✅ | `evaluator.ixx` | set-code/query/mutate 15+ primitives |
| IR 覆盖 | ✅ | `ir.ixx` | 算术、比较、if、let、lambda、pair/quote |
| IR 管线默认启用 | ✅ | `service.ixx` | eval() 统一 IR-first |
| P2996 反射缓存序列化 | ✅ | `cache_reflect.cpp` | auto_serialize via std::meta |
| TypeAnnotation CastOp | ✅ | `lowering_impl.cpp` | 类型标注边界 emit CastOp |
| DeadCoercionElimination | ✅ | `pass_manager.ixx` | 冗余 CastOp 消除 |

## 后续步骤（已完成）

1. ✅ **ArenaGroup 集成到 eval 路径** — `compile_module()` + `unload_module()` + `reload_module()` (fcd95e0)
2. ✅ **模块级增量编译** — ModuleState dirty 追踪 + `mark_module_dirty()` + `reload_module()` (e5393e0)
3. ✅ **磁盘缓存启用** — 通过 mmap 缓存 FlatAST + IRModule (1e6fd2b)
4. ✅ **P2996 编译期反射** — auto_serialize/validate via std::meta
5. ✅ **Incremental CaaS** — cache_serialize_header + cache_validate_header

## 文件清单

```
src/core/arena.ixx               — ASTArena + SmallObjectPool + ArenaGroup
src/compiler/service.ixx         — CompilerService (eval/define/mutate/session)
src/compiler/cache.ixx           — MappedCache + write_cache/open_cache
src/compiler/pass_manager.ixx    — ComputeKind/Arity/ConstFold/TypeCheckWrap
src/compiler/ir.ixx              — IR opcodes + IRModule + IRInterpreter
src/compiler/lowering.ixx        — Expr → IR lowering
src/compiler/evaluator.ixx       — Tree-walker Evaluator
src/main.cpp                     — CLI + --serve
docs/caas_integration.md         — 本文档
```
