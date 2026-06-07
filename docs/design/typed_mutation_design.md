# Typed Mutation Operators —— 已实现

> **Status (2026-06-07, Issue #112):** ✅ 三周计划已全部完成；后续在
> #107/#108/#109/#110 中**重大扩展**：
>
> - **#107 part 1**: `workspace_mtx_` 共享/独占协议
> - **#107 part 3**: `ast:version` 原语
> - **#107 part 4**: 替换 `typecheck-current` 在 4 个 fuzzer 路径
> - **#107 part 5**: DefUseIndex per-sym 失效（详见 §6）
> - **#107 part 6**: 直接 FlatAST snapshot/restore（详见 §7）
> - **#108 part 2**: `ast:defs` / `ast:nodes` 反射
> - **#110**: `mutate:query-and-replace` —— query + replace 组合原语
>
> 本文档已同步到当前实现状态。

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

### 3.1 MutationRecord —— 变异记录

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

### 3.2 MutationLog —— 变异日志

```cpp
// src/core/ast.ixx —— 追加到 FlatAST

// FlatAST 新增字段
std::pmr::vector<MutationRecord> mutation_log_;
std::pmr::vector<uint32_t> node_first_mutation_;  // first mutation index per node
uint64_t next_mutation_id_ = 1;
```

每个 FlatAST 实例持有一个线性增长的事务日志。日志不可变：只追加，不删除。

### 3.3 TypedMutationOp —— 类型化变异算子

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
1. `check_preconditions(old_type, new_value)` —— 类型兼容性检查
2. `create_patches()` —— 生成底层 Patch 列表
3. `apply_patches(patch_list)` —— 应用到 FlatAST
4. `record_mutation(...)` —— 写入 MutationLog
5. `revert()` —— 撤销本次变异的 Patch 列表

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

```cpp
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
│  ├─ value_cache_ (L6.5+)                    │
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
P3  ← 已完成 (Typed Mutation)
  ├── Week 1: MutationRecord + MutationLog
  ├── Week 2: TypedMutationOp 原语
  └── Week 3: Provenance 查询 + AI 协议集成
P4  ← 已完成 (#107/#108/#109/#110)
  ├── workspace_mtx_ 共享/独占协议（#107 part 1）
  ├── ast:version + per-sym 失效（#107 part 3, 5）
  ├── DefUseIndex（#107 part 5）
  ├── Direct FlatAST snapshot/restore（#107 part 6）
  ├── ast:defs / ast:nodes（#108 part 2）
  ├── mutate:query-and-replace（#110）
  ├── Fiber scheduler + work-stealing（#109）
  └── ASAN: 0 leaks on 50-iter snapshot+mutate+restore
P5  ← 进行中
  ├── Capability Effects
  ├── 增量热更新健全性（EDSL V2 cache）
  └── Versioned Types
```

---

## 6. DefUseIndex + WorkspaceTree COW 集成（#107+ 新增）

> **本节是 #112 的 sync 重点 —— 把当前 DefUseIndex 和 WorkspaceTree
> COW 的实装状态反映到本设计文档。**

### 6.1 DefUseIndex 概览

`DefUseIndex`（在 `evaluator_impl.cpp` ~7300 附近）是 `Evaluator` 持有的
派生缓存，索引"哪个表达式定义/使用哪个 sym"。

#### 数据结构

```cpp
struct DefUseIndex {
    // 全局 epoch（monotonic 计数器）
    std::uint64_t global_version_ = 0;

    // 失效的 sym 集合（per-sym 粒度的 stale tracking）
    std::unordered_set<SymId> stale_syms_;

    // defs_[sym] = {def-node-ids}
    // uses_[sym] = {use-node-ids}
    // callers_[fn-sym] = {call-sites}
    // ...
};
```

`stale_syms_` 是 #107 part 5 引入的 per-sym 失效机制，取代了
"全局失效 → 全量 reindex"的旧设计。

#### Touch 协议（任何 mutate 算子必须遵守）

```cpp
// 在 mutate:foo 中 —— 必做 2 步
defuse_affected_syms_.insert(name);     // fallback 路径，永远有效
if (defuse_touch_fn_)                    // fast path（index 已建好时）
    defuse_touch_fn_(defuse_index_, sym);
```

**为什么两步都要**：当 `defuse_touch_fn_` 为空（index 还没建）时，
`defuse_affected_syms_` 仍会被记录；下一次 `ensure_defuse()` 会从
这个 list 重新 build。如果未来某 mutate 路径忘了调 touch，
`defuse_affected_syms_` 是安全网 —— 数据不会 silently 错。

#### 新增 API（#107 part 5）

```cpp
// in DefUseIndex
void touch_sym(SymId s);
void touch_syms(std::span<const SymId> ss);
bool is_sym_stale(SymId s) const;
void mark_sym_fresh(SymId s);
void mark_syms_fresh(std::span<const SymId> ss);
void mark_all_fresh();          // build() 末尾调用
std::size_t stale_count() const;
std::uint64_t current_version() const;
```

#### Query API（暴露给 Aura 端）

```scheme
(query:def-use "fib")        ; → ((def-node-ids) . (use-node-ids))
(query:reaches 21)            ; → 从 def 节点出发查所有 use
(query:effects "fib")         ; → defs + uses + callers
(query:index-stats)           ; → ((stale-syms 0) (defuse-version 7))
(ast:version)                 ; → 7  (workspace AST 版本)
```

### 6.2 WorkspaceTree COW（多 workspace 隔离）

`WorkspaceTree`（`evaluator_impl.cpp` ~7865）支持多层 workspace，
子 workspace 用 **copy-on-write** 共享父的 `FlatAST` / `StringPool`。

#### 数据结构

```cpp
struct WorkspaceNode {
    std::string name;
    aura::ast::FlatAST* flat = nullptr;
    aura::ast::StringPool* pool = nullptr;

    // COW: 父的 flat/pool（无 copy 直到首次 mutate）
    aura::ast::FlatAST* parent_flat_ = nullptr;
    aura::ast::StringPool* parent_pool_ = nullptr;

    std::uint64_t generation = 0;
    bool read_only = false;
    bool has_own_flat = false;  // COW 已触发？
    bool is_root = false;

    // Issue #97 Action 3: per-workspace 内存预算
    std::size_t memory_used = 0;
    std::size_t memory_budget = 0;     // 0 = unlimited
    std::uint64_t cow_refused_count = 0;
};

struct WorkspaceTree {
    std::vector<WorkspaceNode> nodes_;
    std::uint32_t active_idx_ = 0;
};
```

#### COW 触发

```cpp
// 首次 mutate 触发
bool ensure_local_flat(std::uint32_t idx) {
    auto& n = nodes_[idx];
    if (n.is_root) return true;
    if (n.read_only) return false;
    if (n.has_own_flat) return true;
    if (n.parent_flat_) {
        // 估算 clone 成本
        std::size_t parent_bytes = n.parent_pool_->data_size() +
                                   n.parent_flat_->size() * 64;
        if (n.memory_budget > 0 &&
            (n.memory_used + parent_bytes) > n.memory_budget) {
            ++n.cow_refused_count;
            return false;  // 超预算
        }
        // Clone via shallow copy（vectors copy their data）
        n.flat = new FlatAST(*n.parent_flat_);
        n.pool = new StringPool(*n.parent_pool_);
        n.has_own_flat = true;
        n.generation = 1;
        n.memory_used = parent_bytes;
        return true;
    }
    return false;
}
```

#### Aura 端 API

```scheme
(ws:create "sandbox")         ; 创建子 workspace（不复制）
(ws:switch 1)                 ; 切换活跃 workspace
(mutate:rebind "fib" "...")   ; 自动触发 COW
(eval-current)                ; 在 sandbox 里求值
(ws:switch 0)                 ; 切回主 workspace（fib 不变）
(ws:merge 1)                  ; 把 sandbox 合并回主
(ws:lock 1 #t)                ; sandbox 设为 read-only
```

### 6.3 集成到 Mutate 算子

每个 mutate 算子需要：

1. **workspace_mtx_** 的 `unique_lock`（#107 part 1）
2. **workspace 验证**（root 一定有 own flat；child 走 COW 触发）
3. **defuse touch 协议**（§6.1）
4. **mutation_log_ 写入**
5. **mark_dirty_upward**（让 value_cache 失效）
6. **fiber yield at mutation boundary**（让其他 fiber 跑）

详细代码模式见 [`docs/developer/evaluator.md §3`](../developer/evaluator.md#3-mutate-primitives--locking-protocol)。

---

## 7. Direct FlatAST Snapshot/Restore（#107 part 6）

> **实装路径**：`ast:snapshot` 和 `ast:restore` 现在的实现是
> `FlatAST` + `StringPool` 的**深拷贝**（heap-allocated `FlatSnapshot`），
> 而不是 reparse source。

### 旧路径（被替换）

```cpp
// 旧 ast:restore 实现
auto parsed = parse_source(saved_source);   // 重新解析
workspace_flat_ = parsed.flat;              // 替换
// 问题：SymId 不稳定 / mutation_log_ 重置 / value_cache 失效
```

### 新路径（lossless deep copy）

```cpp
// ast:snapshot —— 深拷贝 flat + pool
struct FlatSnapshot {
    std::unique_ptr<FlatAST> flat;
    std::unique_ptr<StringPool> pool;
    std::vector<MutationRecord> mutation_log;
};
std::vector<FlatSnapshot> snapshots_;

EvalValue ast_snapshot(EvalValue name) {
    snapshots_.emplace_back(FlatSnapshot{
        std::make_unique<FlatAST>(*workspace_flat_),
        std::make_unique<StringPool>(*workspace_pool_),
        workspace_flat_->mutation_log_   // copy
    });
    return make_int(snapshots_.size() - 1);
}

EvalValue ast_restore(int snap_id) {
    if (snap_id < 0 || snap_id >= snapshots_.size())
        return make_bool(false);

    auto& snap = snapshots_[snap_id];
    *workspace_flat_  = *snap.flat;     // deep copy back
    *workspace_pool_  = *snap.pool;

    // 重置派生缓存
    defuse_index_destroy(&defuse_index_);
    defuse_affected_syms_.clear();
    mark_all_defines_dirty_fn_();
    pre_cache_workspace_defines_fn_();

    return make_bool(true);
}
```

### Lossless 含义

| 状态 | 跨 restore 保留？|
|------|------------------|
| SymId identity | ✅（strings 在 pool 里的顺序不变）|
| `mutation_log_` | ✅（深拷贝）|
| `type_id_` | ✅（不需要 re-typecheck）|
| `value_cache_` | ✅（eval-current 缓存仍命中）|
| `defuse_index_` | ❌（重 build，需要但廉价）|

### 失败路径

OOM 时 `ast:snapshot` 把 source string 也存一份（`has_flat=false`）；
`ast:restore` 优先用 deep-copy，失败时回退到 reparse source。

---

## 8. 反思与教训

### 8.1 三周计划中没考虑到的

- **并发 Agent 的 workspace 共享**：第 6 节的 `workspace_mtx_` 是后加的
  （#107 part 1）。原设计假定单线程 REPL + serve 协议（§2.2），多 Agent
  并发 mutate 暴露了 read/write race。
- **DefUseIndex 的失效粒度**：原设计是全量失效 → 全量 rebuild（每次
  mutate 后 ~500μs）。在高频 mutate 场景下成为瓶颈。Per-sym 失效
  （#107 part 5）把单次 mutate 的 defuse 重 build 降到 O(stale_syms)。
- **Self-modifying-flat 迭代 bug**：#110 的 qar 暴露了"iterate while
  grow"模式。详见 [`docs/developer/evaluator.md §1`](../developer/evaluator.md#1-the-self-modifying-flat-iteration-rule-issue-111-lesson)。

### 8.2 三个失败的设计选择

1. **按 `TypeId` 而非 string 存 MutationRecord.old/new_type**
   - 原因：TypeRegistry 每次 typecheck 重建，TypeId 不跨调用。
   - 修正：MutationRecord 存 string 化后的 type（`format_type(old_tid)`）。
   - 详见 §9.5 原来的 open question。

2. **reparse-based snapshot**
   - 原因：SymId 不稳定 + value_cache 失效。
   - 修正：直接 deep-copy FlatAST（#107 part 6）。

3. **TypeRegistry 不跨调用持久化**
   - 原因：每次 typecheck 重建。
   - 修正（未做）：考虑让 TypeRegistry 实例挂在 Evaluator 上，跨 typecheck
     复用 —— 但目前规模下重建 ~1ms 可接受。

### 8.3 当前 open

- 🟡 **增量类型检查**：mutate 后只类型检查 dirty 子树（当前是全量
  ~1-5ms，足够；如需更细粒度，从 source-hash 缓存开始）
- 🟡 **MutationLog Aura API**：暴露给 Aura 端做复杂回滚
  （`ast:diff 0` 已能给出结构化 diff，但精确 revert-by-id 还没暴露）
- 🟡 **AutoFixEngine**：基于规则的自动修复
  （`std/rule.aura` 是基础，集成 `mutate:*` 后可以做更复杂修复）

---

## 9. 开放在设计阶段的问题（已大部分解决）

1. **MutationRecord 的存储粒度**：每个节点一个记录链 vs 每个 FlatAST
   全局日志？✅ 当前选全局日志 + `node_first_mutation_` 索引
   （类似 `child_begin_` 的结构），查询时只需要 scan 该节点的记录。

2. **回滚的粒度**：按 `mutation_id` 回滚 vs 按时间戳回滚？✅
   `rollback_mutation(mid)` 按 id，`rollback_all_since(mid)` 按 id 范围
   （id 单调递增 → 等价于"从该点之后全部回滚"）。

3. **并发变异**：✅ #107 part 1 实装 `workspace_mtx_` 共享/独占协议 +
   `MutationBoundary yield`（让其他 fiber 在 mutate 之前跑，避免 starvation）。

4. **Tombstone 记录 vs 物理删除**：✅ 回滚时保留 MutationRecord 但标记
   `RolledBack` 状态。不删除记录，保证审计链完整。

5. **类型 ID（TypeId）的跨 session 稳定性**：✅ MutationRecord 中存储
   `format_type(old_tid)` / `format_type(new_tid)` 字符串，而不是
   TypeId。TypeId 仅作为 in-session 优化。

---

## 10. 相关文档

- [`docs/issue-closings/107-closing.md`](../issue-closings/107-closing.md) — workspace mutex + AST versioning + direct snapshot
- [`docs/issue-closings/110-closing.md`](../issue-closings/110-closing.md) — qar + self-modifying-flat 教训
- [`docs/issue-closings/111-closing.md`](../issue-closings/111-closing.md) — self-modifying-flat 审计
- [`docs/developer/evaluator.md`](../developer/evaluator.md) — evaluator 开发者指南（C++ 实装细节）
- [`docs/design/defuse_analysis.md`](defuse_analysis.md) — DefUseIndex 内部数据布局
- [`docs/design/query_edsl_design.md`](query_edsl_design.md) — Query + Transform EDSL 全貌
