# Typed Mutation Operators — 已实现

> **✅ 三周计划已全部完成 (2026.05.15)**
> - W1: MutationRecord + MutationLog (FlatAST 存储)
> - W2: TypedMutationOp 原语 (check-preconditions/replace-type)
> - W3: Provenance + --serve + AI 协议集成
> - 55/55 测试，~10800 行代码

## 1. 背景

| 组件 | 能力 | 限制 |
|------|------|------|
| `Patch` | 按 `(node_id, field_offset, new_value)` 直接改 FlatAST SoA 字段 | 无类型检查、无来源追踪、无事务 |
| `TransformEngine` | 模式匹配 (QueryEngine) + 替换 (parse_replace → generate_patches) | 纯语法替换，不感知类型语义 |
| `apply_patches` | 批量应用 Patches 到 FlatAST | 一次性不可回滚 |

核心问题：**AI 修改代码的路径没有类型层防护。**

---

## 2. 设计目标

### 2.1 核心诉求

1. **带类型的变异**：每次修改通过 `check_mutation(old_type, new_code)` 验证类型兼容性
2. **来源追踪**：每个节点知道"谁在什么时候为什么修改了它"
3. **事务语义**：变异原子化 —— 要么全部成功，要么全部回滚
4. **可审计进化**：支持查询"这个节点经过了多少次变异？每次是什么？"

### 2.2 非目标

- Phase 1 不做完整 Hindley-Milner 风格的类型方案实例化（已有 forall 骨架，渐进补齐）
- Phase 1 不做跨模块变异的能力检查（留给 Capability Effects Phase 2）

---

## 3. 架构设计

### 3.1 MutationRecord — 变异记录

新增数据结构，关联到每个节点的演化历史：

```cpp
// src/core/ast.ixx

export struct MutationRecord {
    uint64_t mutation_id;       // 全局单调递增 ID
    uint64_t timestamp_ms;      // 变异发生时间
    NodeId target_node;         // 被修改的节点
    std::string operator_name;  // 变异算子名称 ("replace-type", "refine-constraint", ...)
    TypeId old_type;            // 变异前的类型 (0 = unknown/untyped)
    TypeId new_type;            // 变异后的类型
    std::string summary;        // 人类可读的变更说明
    MutationStatus status;      // committed / rolled_back
};

export enum class MutationStatus : uint8_t {
    Committed,
    RolledBack,
};
```

### 3.2 MutationLog — 变异日志

```cpp
// src/core/ast.ixx — 追加到 FlatAST

// FlatAST 新增字段
std::pmr::vector<MutationRecord> mutation_log_;
std::pmr::vector<uint32_t> node_first_mutation_;  // first mutation index per node
uint64_t next_mutation_id_ = 1;
```

每个 FlatAST 实例持有一个线性增长的事务日志。日志不可变：只追加，不删除。

### 3.3 TypedMutationOp — 类型化变异算子

变异不是直接写 `Patch`，而是通过一组预定义的有类型算子：

```
(mutate:replace-type node new-type)
(mutate:replace-children node child-indices new-child-values)
(mutate:refine-constraint node constraint-expr)
(mutate:insert-child parent-node position child-node)
(mutate:delete-child parent-node position)
(mutate:swap-nodes node-a node-b)
(mutate:wrap-node parent-wrapper inner-node)
```

每个算子内部：
1. `check_preconditions(old_type, new_value)` — 类型兼容性检查
2. `create_patches()` — 生成底层 Patch 列表
3. `apply_patches(patch_list)` — 应用到 FlatAST
4. `record_mutation(...)` — 写入 MutationLog
5. `revert()` — 撤销本次变异的 Patch 列表

### 3.4 类型兼容性检查

`check_mutation` 的核心逻辑：

```
check_mutation(old_node_id, op_name, new_value) → Result<void, MutationError>

规则：
1. 如果新节点是 LiteralInt → 新类型必须是 old_type 的 subtype
2. 如果新节点是 Call → 函数签名的返回类型必须是 old_type 的 subtype
3. 如果 new_value 包含自由变量 → 必须在作用域中已绑定
4. 如果 old_node 有 Occurrence 细化 → 新节点必须保持或增强该细化
5. 如果 old_type 有 Refinement 约束 → 新代码必须 preserve 该约束
```

错误类型：

```cpp
export enum class MutationErrorKind : uint8_t {
    TypeMismatch,
    LostOccurrenceRefinement,
    UnboundVariable,
    ArityMismatch,
    WouldBreakBlame,
    ProvenanceConflict,
};
```

### 3.5 最小实现路径 (Phase 1)

**Week 1: MutationRecord + MutationLog**

```cpp
// FlatAST 新增
MutationRecord add_mutation(NodeId node, std::string_view op_name,
                            TypeId old_type, TypeId new_type,
                            std::string_view summary);

std::span<const MutationRecord> mutation_history(NodeId node) const;

bool has_mutations(NodeId node) const;
uint64_t mutation_count(NodeId node) const;
```

**Week 2: TypedMutationOp 原语**

在 evaluator 中注册为 primitives：

```rust
// (mutate:replace-type node-id type-id)
//   1. 查找 node-id 的当前类型
//   2. check_mutation(old_type, new_type)
//   3. 写 Patch + 写 mutation_log

// (mutate:wrap-node wrapper-sexpr inner-node-id)
//   1. parse wrapper-sexpr 为 FlatAST
//   2. 找到 wrapper 中的占位符 (hole)
//   3. 将 inner-node 关联的子树移动到占位位置
//   4. 将 wrapper 的根节点替换原 inner-node
//   5. 类型：wrapper 的返回类型必须兼容 inner-node 的 old_type
```

**Week 3: Provenance 查询**

```
(query (provenance-of node-id))
  → [{mutation_id, operator, old_type, new_type, timestamp, status}, ...]

(query (mutation-log :since timestamp :limit 100))
  → 最近的 N 条变异记录
```

---

## 4. 与现有系统的集成

### 4.1 FlatAST 变更

```
┌─────────────────────────────────────────────┐
│  FlatAST (现有)                              │
│  ├─ tag_ / int_val_ / sym_id_ / ...         │
│  ├─ child_data_ / param_data_               │
│  ├─ type_id_ (L6.5+)                        │
│  └─ marker_ (SyntaxMarker, 卫生宏)           │
├─────────────────────────────────────────────┤
│  FlatAST (新增)                              │
│  ├─ mutation_log_        ← MutationRecord[] │
│  ├─ node_first_mutation_ ← uint32_t[]       │
│  └─ next_mutation_id_    ← uint64_t         │
└─────────────────────────────────────────────┘
```

### 4.2 CompilerService 集成

```cpp
// src/compiler/service.ixx

class CompilerService {
    // ... 现有方法 ...

    // 类型安全变异
    MutationResult typed_mutate(
        NodeId target,
        std::string_view op_name,
        std::string_view new_code
    );
    
    // 变异日志查询
    MutationLogEntry query_mutation(NodeId node_id) const;
    std::vector<MutationLogEntry> query_mutation_log(MutationQuery filter) const;
    
    // 事务回滚
    bool rollback_mutation(uint64_t mutation_id);
    bool rollback_all_since(uint64_t since_mutation_id);
};
```

### 4.3 AI Agent 协议集成

当前 `--serve` 协议新增命令：

```json
{"cmd": "typed-mutate", "op": "replace-type", "node": 42, "code": "(Int)"}
  → {"status": "ok", "mutation_id": 7, "new_type": "Int"}

{"cmd": "mutation-log", "node": 42}
  → {"status": "ok", "log": [{"id": 7, "op": "replace-type", ...}]}

{"cmd": "rollback", "mutation_id": 7}
  → {"status": "ok", "rolled_back": true}
```

---

## 5. 与 Roadmap 的衔接

```
P0  ← 已完成
  └── Float 支持
P1  ← 已完成
  ├── 类型推断加固
  ├── 错误体验
  └── 闭包生命周期修复
P2  ← 已完成
  └── forall 多态
P3  ← 当前阶段 (Typed Mutation)
  ├── Week 1: MutationRecord + MutationLog
  ├── Week 2: TypedMutationOp 原语
  └── Week 3: Provenance 查询 + AI 协议集成
P4  ← 下一阶段
  ├── Capability Effects
  ├── 增量热更新健全性
  └── Versioned Types
```

---

## 6. 开放在设计阶段的问题

1. **MutationRecord 的存储粒度**：每个节点一个记录链 vs 每个 FlatAST 全局日志？当前选全局日志 + `node_first_mutation_` 索引（类似 child_begin_ 的结构），查询时只需要 scan 该节点的记录。

2. **回滚的粒度**：按 `mutation_id` 回滚 vs 按时间戳回滚？先按 `mutation_id`，因为 id 单调递增，可以支持 `rollback_from(bad_mutation_id)` 回滚到该点之前。

3. **并发变异**：当前不考虑并发 —— Aura 是单线程 REPL + --serve 协议。如果以后需要并发 Agent 协作变异，再加 MVCC 层。

4. **Tombstone 记录 vs 物理删除**：回滚时保留 MutationRecord 但标记 `RolledBack` 状态。不删除记录，保证审计链完整。

5. **类型 ID（TypeId）的跨 session 稳定性**：当前 TypeRegistry 每次 `typecheck` 调用都重建，TypeId 不跨调用。MutationRecord 中存储的 `old_type` 和 `new_type` 需要序列化为字符串（`format_type` 的结果）而不是 TypeId。
