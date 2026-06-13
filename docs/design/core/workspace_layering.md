# Workspace 分层隔离 — Design

**Status**: Design (2026-05-25)
**Design Author**: Ani
**Driver**: 多 Agent 协作 + 安全沙箱 + 分支实验（EDSL Roadmap W5-6）

---

## 0. Implementation Status (2026-06-11, Issue #156)

**重要**：本文档的 **P0 已实装**（独立子 workspace + 切换 + 列表 + 删除 + read-only stub），P1/P2 仍为设计稿。准确分两层：

### C++ Core Layer (`src/compiler/evaluator.ixx` / `evaluator_impl.cpp`)

| 组件 | 实装 | 备注 |
|------|------|------|
| `WorkspaceTree` 数据结构 | ✓ | `evaluator_impl.cpp` ~7865，SoA + COW + per-workspace memory budget |
| `WorkspaceNode` (parent/children/read_only/transient/has_local) | ✓ | 完整字段，COW 触发逻辑同 §3.1 |
| `create_child` + `delete_child` + `is_read_only` | ✓ | P0 实现 |
| `set_active` + `resolve_flat` / `resolve_pool` | ✓ | 切换时同步 `workspace_flat_` / `workspace_pool_` 指针 |
| `ensure_local_flat` (COW 触发) | ✓ | 含 `memory_budget` 检查 + `cow_refused_count` 统计（#97 Action 3）|
| COW 深度克隆（FlatAST + StringPool）| ✓ | `std::vector` 元素深拷贝 + pool copy |
| 跨层 NodeId 分配 | ✗ (设计) | §8 Open Issue 1 — 当前依赖 whole-clone 兼容，mutate 后子层 NodeId 不再对应父层 |
| StringPool 一致性（不同 pool 的 intern 结果）| ✗ (设计) | §8 Open Issue 2 — 查询需用名字而非 SymId |
| 跨 workspace 读写锁 | ✗ (设计) | §8 Open Issue 3 — 当前单线程，多 Agent 共享需 reader-writer lock |
| `merge` 三路合并 | ✗ (设计) | §7 P2 — 当前只有文本级 `merge` |

（Refactor 2.3/3.2/5.1 note）ADT 提取和 merr 消除已完成（见 adt_runtime 和 make_merr 变更）；evaluator_impl 精简中。详见 developer/evaluator.md 和 roadmap。
（5.2 note）register_primitives 签名在 adt/ffi 间已对齐（coverage_counters 等指针一致传递）。
（Phase 2 pilot-2 note）CMake helper `aura_add_issue_test` 增强并转换首个真实 test_issue_132；后续小步将继续 dedup 40+ 测试。详见 evaluator.md §12。
（pilot-3 note）base 增加 value/type_checker 相关；转换 test_issue_116。

### Aura Layer (`lib/std/workspace.aura`)

| Helper | 实装 | 备注 |
|--------|------|------|
| `workspace:create` | ✓ | P0 — 创建子 workspace (COW) |
| `workspace:switch` | ✓ | P0 — 切换 active 层 |
| `workspace:delete` | ✓ | P0 — 删除子层 |
| `workspace:list` | ✓ | P0 — 列出所有层 |
| `workspace:current` | ✓ | P0 — 返回 active id |
| `workspace:lock` | ✓ (read-only) | P1 — 标记 active 层 read-only；mutate 入口检查返回 false |
| `workspace:can-write?` | ✓ | P1 — 权限查询 |
| `workspace:sync-from` | ✗ (设计) | P2 — 选择性拉取符号 |
| `workspace:discard` | ✗ (设计) | P2 — 丢弃变更不合并 |
| `workspace:merge` | △ (仅文本级) | P0/P2 文档 P1 mutation_log 三路合并未做 |

### Future Work

- **跨 workspace 内存预算**（`memory_budget`）：已实装 per-workspace 跟踪 + `cow_refused_count` 统计；全局 GC / 自动 scale-out 未做
- **跨 host workspace 同步**：当前是进程内，跨 host 需序列化 mutation_log + 增量同步协议
- **三路合并**：当前是简单文本级 `merge`，P1 的 mutation_log 三路合并未实现

**AI Agent 读者请注意**：本文档是设计意图的权威来源。P0 实装代码见 `evaluator_impl.cpp` 的 `WorkspaceTree` / `WorkspaceNode` 章节；P1/P2 部分（如 `workspace:sync-from` / 三路合并）尚未实装，写代码前查实装状态。

---


## 1. Problem

当前 `Evaluator` 持有单个 `workspace_flat_`，所有 Agent 共享同一块 AST：

```
Evaluator
  └── workspace_flat_  ← 所有 mutation 直接作用于此
```

这意味着：
- Agent A 做危险实验时，必须 `ast:snapshot` → mutate → 崩了 → `ast:restore`（文本级重 parse，丢失增量缓存）
- 无法让一个 Agent 只读锁定某些函数，另一个 Agent 在隔离副本上工作
- 没有"分支 / 合并"概念——要么改，要么不改

## 2. 约束

| 约束 | 来源 | 方案影响 |
|:-----|:-----|:--------|
| 零成本抽象 | 不产生额外延迟 | 读路径应直接穿透父层 |
| 增量编译兼容 | 脏标记、arena cache | 每层有自己的 dirty 状态 + 编译结果缓存 |
| 扁平 AST | `FlatAST` 是 SoA vector | 层的 flat 彼此独立，NodeId 不跨层共享 |
| 向后兼容 | 现有代码不改 | 单 workspace 模式继续工作，分层是可选升级 |
| 所有权明确 | 每层知道哪些节点是自己的 | COW + 引用计数 |

## 3. 核心数据结构

### 3.1 WorkspaceNode —— 层节点

```cpp
struct WorkspaceNode {
    // ── 本层状态 ──
    std::string name;
    FlatAST* flat = nullptr;       // 本层的 FlatAST（如果本地有 mutate）
    StringPool* pool = nullptr;     // 本层的 StringPool
    std::uint64_t generation = 0;  // 本层的变更版本号

    // ── 层级关系 ──
    WorkspaceNode* parent = nullptr;      // 父层（只读）
    std::vector<WorkspaceNode*> children; // 子层（本层可读写）

    // ── 权限 ──
    bool read_only : 1 = false;   // 禁止对本层 mutate
    bool transient : 1 = false;   // 临时层，删除自动清理
    bool has_local : 1 = false;   // 是否有本地 flat（COW 延迟创建）

    // ── 验证 ──
    // lookup(sym): 本层 flat → parent flat → grandparent ...
    // mutate(node): 只能在本地 flat 上操作；父层的节点需先 COW 到本地
};
```

### 3.2 存储布局：WorkspaceTree

```cpp
class WorkspaceTree {
public:
    WorkspaceTree() { roots_.push_back({}); }  // 初始根层

    // ── 生命周期 ──
    std::uint32_t create_child(std::string_view name,
                               std::uint32_t parent_idx = 0,
                               bool copy_on_write = true);

    bool delete_child(std::uint32_t idx);
    bool is_read_only(std::uint32_t idx) const;
    void set_read_only(std::uint32_t idx, bool ro);

    // ── 查找 ──
    WorkspaceNode* active() { return active_; }
    void set_active(std::uint32_t idx);

    // ── 数据访问 ──
    FlatAST* flat(std::uint32_t idx);       // 获取 idx 层的 flat（惰性 COW）
    FlatAST* resolve_flat();                // 当前 active 层的 flat
    StringPool* resolve_pool();             // 当前 active 层的 pool

private:
    std::vector<WorkspaceNode> nodes_;
    WorkspaceNode* active_ = nullptr;

    // COW 实现
    FlatAST* ensure_local_flat(std::uint32_t idx);
};
```

### 3.3 路径：逐层穿透

```
lookup(x) on child-ws:

  1. child.flat 有 x? → return
  2. parent.flat 有 x? → return (read-only)
  3. grandparent.flat 有 x? → return
  4. ... → unbound

写入:

  mutate on child-ws:
    node 属于 child? → 直接改
    node 属于 parent? → copy-on-write 到 child.flat → 改 child 的副本
    node 属于父层且 child 是 read-only? → PermissionDenied
```

## 4. API 设计

### 4.1 创建与切换

```
(workspace:create name [parent-id])    → workspace-id
(workspace:switch id)                  → #t / #f
(workspace:delete id)                  → #t / #f
(workspace:current)                    → id
(workspace:list)                       → ((id name) ...)
```

#### 实现（P0 版本）

```cpp
// Evaluator::Evaluator() 中注册
primitives_.add("workspace:create", [this](const auto& a) -> EvalValue {
    if (!workspace_tree_)
        workspace_tree_ = new WorkspaceTree();

    std::string name;
    if (a.size() >= 1 && is_string(a[0]))
        name = string_heap_[as_string_idx(a[0])];

    std::uint32_t parent_id = 0;
    if (a.size() >= 2 && is_int(a[1]))
        parent_id = static_cast<std::uint32_t>(as_int(a[1]));

    auto id = workspace_tree_->create_child(name, parent_id);
    return make_int(static_cast<std::int64_t>(id));
});
```

P0 `create_child` 实现：

```cpp
std::uint32_t WorkspaceTree::create_child(std::string_view name,
                                           std::uint32_t parent_idx,
                                           bool copy_on_write) {
    auto& parent = nodes_[parent_idx];
    auto idx = nodes_.size();

    WorkspaceNode child;
    child.name = name;
    child.parent = &parent;
    child.has_local = !copy_on_write;  // COW 模式延迟创建 flat
    child.read_only = false;
    child.transient = false;

    if (!copy_on_write && parent.flat) {
        // 立即复制 flat（用于独立实验）
        child.flat = new FlatAST(*parent.flat);
        child.pool = new StringPool(*parent.pool);
        child.has_local = true;
    }

    nodes_.push_back(std::move(child));
    parent.children.push_back(&nodes_.back());
    return idx;
}
```

#### 切换层

```cpp
primitives_.add("workspace:switch", [this](const auto& a) -> EvalValue {
    if (a.empty() || !is_int(a[0]) || !workspace_tree_)
        return make_bool(false);
    auto idx = static_cast<std::uint32_t>(as_int(a[0]));
    if (idx >= workspace_tree_->size())
        return make_bool(false);

    workspace_tree_->set_active(idx);

    // 同步到 evaluator 的工作指针
    auto* ws = workspace_tree_->active();
    workspace_flat_ = ws->flat;    // 如果有 local flat
    workspace_pool_ = ws->pool;    // 如果有 local pool
    defuse_index_ = nullptr;       // 缓存失效，自动重建

    return make_bool(true);
});
```

### 4.2 权限控制

```
(workspace:lock id [#t/#f])           → 设置/取消只读
(workspace:can-write? id [node-id])   → 是否能改指定节点
```

`lock` 实现：在 `WorkspaceNode` 上设 `read_only` 标记。所有 `mutate:*` 原语插入 `workspace_tree_` 检查：

```cpp
// 在每个 mutate 原语入口添加
if (workspace_tree_) {
    auto* ws = workspace_tree_->active();
    if (ws && ws->read_only)
        return make_bool(false);  // PermissionDenied
}
```

### 4.3 选择同步

```
(workspace:sync-from source-id symbol-name)
  → #t / #f
  从 source workspace 拉取指定符号的定义到当前 workspace
```

实现思路：在 source workspace 的 flat 中 query 到 symbol 的定义节点 → 获取其源码（通过 `current-source`） → 解析到当前 workspace → 用 `mutate:rebind` 或 `set-code` 整合。

### 4.4 分发后清理

```
(workspace:discard id)        → 丢弃子 workspace（不合并）
(workspace:merge id)          → 将子 workspace 的变更合并到父级
```

P0 合并策略：将子 workspace 的 `current-source` 合并到父级（简单文本级）。P1 使用 mutation_log 做三路合并。

## 5. 增量编译集成

每层维护自己的 `ArenaGroup`（编译缓存）：

```cpp
struct WorkspaceNode {
    // ...
    struct CompileCache {
        std::unordered_map<std::string, void*> ir_cache;  // module → IR
        std::vector<bool> dirty_modules;
    };
    std::unique_ptr<CompileCache> compile_cache;
};
```

**查询路径：**
- 当前层有 `ir_cache[module]` → 直接用
- 没有 → 查父层 → 父层有 → 只读引用（TODO: copy-on-write 如果本层要改）
- 都没有 → 从本层 flat 编译 → 缓存到本层

## 6. 与现有系统的集成

### 6.1 evaluator.ixx 变化

```cpp
class Evaluator {
    // 新增
    void* workspace_tree_ = nullptr;  // WorkspaceTree*

    // 现有
    FlatAST* workspace_flat_ = nullptr;  // 指向 active workspace 的 flat
    StringPool* workspace_pool_ = nullptr;
};
```

`workspace_flat_` 和 `workspace_pool_` 继续存在，但指向当前 active 层的 flat/pool。单 workspace 模式下（没有 `workspace_tree_`），行为不变。

### 6.2 对现有 mutate 原语的影响

每个 `mutate:*` 和 `set-code` 原语入口加一层检查：

```cpp
// Before: 直接使用 workspace_flat_
// After:
if (workspace_tree_) {
    auto* ws = workspace_tree_->active();
    if (!ws || ws->read_only)
        return make_bool(false);
}
// 继续使用 workspace_flat_（已指向 active 层的 flat）
```

### 6.3 对 def-use 的影响

每个 workspace 层有独立的 DefUseIndex（延迟构建）。切换 workspace 层时 `defuse_index_ = nullptr`，自动重建。

### 6.4 对 snapshot 的影响

`ast:snapshot` 只捕获当前 active 层的状态。分层后，每层可以独立 snapshot。

## 7. 实现路标

### P0 — 独立子 workspace（1 天）

| 任务 | 说明 |
|:----|:------|
| `WorkspaceTree` struct | 树结构 + COW 创建 |
| `workspace:create` | 基于 COW 或全量 copy 创建子层 |
| `workspace:switch` | 切换 active 层，同步 flat/pool 指针 |
| `workspace:list` | 列出所有层 |
| `workspace:delete` | 删除子层 |
| 权限 stub | `read_only` 标记 + mutate 入口检查 |

**P0 验收**：
```
(set-code "(define (f x) (+ x 1))")
(define w1 (workspace:create "experiment"))
(workspace:switch w1)
;; 在子层 mutate — 不影响主层
(mutate:rebind "f" "(lambda (x) (* x 2))" "test")
(current-source)  → "(define f (lambda (x) (* x 2)))"

(workspace:switch 0)  ;; 切回根层
(current-source)  → "(define (f x) (+ x 1))"  ;; 没被影响
```

### P1 — COW + 只读穿透 + 权限（2-3 天）

| 任务 | 说明 |
|:----|:------|
| 只读父层穿透 | 读操作走 parent chain |
| `workspace:lock` | 设置/取消只读 |
| `workspace:can-write?` | 权限查询 |
| COW 延迟创建 | 子层未 mutate 时不复制 flat |

### P2 — 同步 + 合并（2-3 天）

| 任务 | 说明 |
|:----|:------|
| `workspace:sync-from` | 选择性拉取符号定义 |
| `workspace:discard` | 丢弃变更 |
| `workspace:merge` | 文本级合并（P0）/ 三路合并（P1） |

## 8. 开放问题

1. **跨层 NodeId 分配**：当前 NodeId 是 FlatAST 数组下标。创建子 workspace 时（COW 复制），子层的 NodeId 与父层兼容（因为 whole-clone）。但 mutate 后，子层的新 NodeId 不再对应父层。需要在 API 层避免跨层 NodeId 泄漏。

2. **StringPool 一致性**：`intern("x")` 在不同 StringPool 实例返回不同 SymId。COW 复制时可以保持一致（复制整个 pool），但独立创建的 layer 需要不同的 intern 结果。查询时用名字而非 SymId。

3. **并发安全**：当前是单线程。多 Agent 共享 workspace tree 时需要读写锁（读穿透 + 写 COW 符合 reader-writer lock 模式）。

4. **合并策略**：mutation_log 包含每层所有变更。merge 时按时间戳排序 event，用三路合并（parent + child_a + child_b）处理冲突。
