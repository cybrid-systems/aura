# Scope-Level Def-Use Chain — Design

> **注意（历史文档）**：本文档是 DefUseIndex 的早期设计探索（2026-05）。当前 C++ 层已完整实现（`query:def-use` 等 11 个 query 原语 + per-sym 失效），Aura 表面通过 `std/query.aura` 提供部分 helper。详见 `design/core/query_edsl.md` §0（Implementation Status）和 `developer/evaluator.md` §4。

**Status**: Design (2026-05-25)
**Design Author**: Ani
**Driver**: `query:def-use`, `query:reaches`, `query:effects`（EDSL Roadmap W1-2）

## 1. Problem

当前 query 系统可以做结构化树查询（node-type, callee, exists, has-type 等），但无法回答：

- "哪个表达式定义了变量 `n`？"
- "改 `render-loop` 的 body 会影响到哪些使用点？"
- "变量 `x` 的 use 点在哪儿？"

这些是 **def-use chain** —— 变量定义到使用的连接。没有它，Agent 在 `mutate:set-body` 前无法判断影响范围，导致语义盲区。

## 2. 约束

| 约束 | 来源 | 方案影响 |
|:-----|:-----|:--------|
| 增量更新 | AST 被 mutate 后只重建局部 | 不能全量 scan |
| 扁平 AST | `FlatAST` 是 vector，NodeId 是下标 | 索引必须是 NodeId 可逆的 |
| 不引入 SSA | IR 是 flat instr，无 phi | 只在 lexical scope 内分析 |
| 多后端共存 | tree-walker + IR + JIT | def-use 建在 AST 层（tree-walker 侧） |
| 性能敏感 | `query:def-use` 可能被高频调用 | 缓存 + 增量 + 冷热分离 |

## 3. 核心数据结构

### 3.1 Lexical Scope Tree

从 FlatAST 中提取 scope 边界。每个 `lambda`, `let`, `letrec`, `begin` 创建新 scope。`define` 绑定到包含它的最近 scope（通常是 module-level 或 lambda）。

```cpp
struct ScopeNode {
    NodeId node;                    // 创建此 scope 的 AST 节点
    std::uint32_t parent;           // 父 scope （按 index，非 NodeId）
    std::uint32_t first_child;      // 子 scope 区间的起点（在 scopes_ 中）
    std::uint32_t child_count;      // 子 scope 数量
    // ── 符号绑定 ──
    std::uint32_t def_first;        // 本 scope 定义的符号在 defs_ 中的偏移
    std::uint16_t def_count;        // 本 scope 定义了 n 个符号
    std::uint32_t sym_first;        // 本 scope 引用的符号（use）在 refs_ 中的偏移
    std::uint16_t sym_count;        // 本 scope 引用了 n 个不同符号
    std::uint32_t use_first;        // 本 scope 的 use 点在 uses_ 中的偏移
    std::uint32_t use_count;        // 本 scope 包含 n 个 use 点
    // ── 脏标记 ──
    bool dirty : 1;                 // 需要重建
    bool has_children : 1;          // 有子 scope
};
static_assert(sizeof(ScopeNode) <= 40);  // 5 个 cache line 内
```

### 3.2 平铺 Arena 布局

所有 scope 数据在三个连续向量中，不做堆分配，不做链表：

```
scopes_:     [module] [λ fib] [let a] [λ helper] ...
               │         │       │        │
               │   parent=0  parent=1  parent=1
               │         │              │
               │    ┌────┘              └──── child_count=1
               │    │                          │
               │  child=2                   child=3

defs_:     [fib] [n] [a] [helper] [x] ...
            │     │   │     │       │
          scope0  s1  s1   s0      s2

uses_:     [fib:1] [fib:2] [n] [n] [a] [helper] ...
            │       │       │   │   │     │
          scope0  scope0  s0  s0  s1   scope0

refs_:     [{sym=fib, first_use=0}] [{sym=n, first_use=2}] ...
            │                          │
          scope0                      scope1
```

### 3.3 主索引（查询入口）

```cpp
struct DefUseIndex {
    // ── Scope Tree ──
    std::vector<ScopeNode> scopes_;
    std::vector<NodeId> scope_roots_;  // 所有顶层 scope 节点（方便 rebuild 入口）

    // ── Def 数据 ──
    // defs_ 是 (SymId, NodeId) 对，按 scope 分组
    // def_syms_ 和 def_nodes_ 同 length，i 处是一对
    std::vector<SymId> def_syms_;      // 定义的符号
    std::vector<NodeId> def_nodes_;    // 定义所在的 NodeId

    // ── Use 数据 ──
    // uses_ 是所有 use 点的 NodeId，按 scope 分组且每个 scope 内按 sym 连续
    std::vector<NodeId> uses_;         // 所有 use 点

    // ── Sym 引用索引（快速 sym→use 跳跃）──
    // refs_ 按 scope 分组，每个条目指向该 scope 内引用的一个 sym
    struct SymRef {
        SymId sym;                     // 引用的符号
        std::uint32_t use_start;       // 在 uses_ 中的偏移（该 sym 在此 scope 的 uses）
        std::uint16_t use_count;       // 此 scope 内该 sym 的 use 数
        std::uint16_t _pad;
    };
    std::vector<SymRef> refs_;

    // ── Sym → All Scopes（跨 scope 跳转）──
    // 按 SymId 索引：哪些 scope 引用/定义了这个 sym
    struct SymScopeRef {
        std::uint32_t scope_index;     // 哪个 scope
        bool is_def : 1;               // 是定义还是引用
        uint32_t local_index : 31;     // 在 defs_ 或 refs_ 中的本地偏移
    };
    // sym_scopes_ 按 SymId 分组平铺
    std::vector<SymScopeRef> sym_scopes_;
    // sym_to_scopes_ 给出 sym_id 在 sym_scopes_ 中的区间
    struct SymScopeRange {
        std::uint32_t start;
        std::uint16_t count;
    };
    std::vector<SymScopeRange> sym_to_scopes_;  // index by SymId

    // ── 元数据 ──
    std::uint32_t total_scopes_ = 0;
    std::uint32_t total_defs_ = 0;
    std::uint32_t total_uses_ = 0;
    std::uint64_t generation_ = 0;     // 每次重建递增
    bool built_ = false;
};
```

## 4. 构建算法

### 4.0 Scope Extraction（递归建立 scope tree）

```
Input: FlatAST, 起始 NodeId，父 scope index
Output: 填充 scopes_

extract_scope(flat, start_id, parent_idx, depth):
  v = flat.get(start_id)
  switch v.tag:
    Lambda | Let | LetRec | Begin:
      创建新 ScopeNode
      node = start_id
      parent = parent_idx
      收集所有绑定的符号 → defs_
      收集其他符号引用 → uses_
      对每个子节点递归 extract_scope
      确定子 scope 区间

    IfExpr:
      对 cond / then / else 分支分提取 scope
    
    Call:
      如果是 define/mutate:set-body 等特殊形式，特殊处理符号绑定
      否则正常遍历子节点

    _:
      遍历所有 children，递归
```

符号绑定检测规则：

| AST 节点 | 定义的 sym | 绑定的位置 |
|:---------|:-----------|:----------|
| `(lambda (x y) ...)` | x, y | lambda 的直接参数 |
| `(let ((x v) ...) ...)` | x, ... | let 子句的 car |
| `(letrec ((x v) ...) ...)` | x, ... | letrec 子句的 car |
| `(define (fn args) body)` | fn | define 的第一个 symbol |
| `(define x expr)` | x | define 的第一个 symbol |
| `(mutate:rebind name (fn (args) body))` | name | mutate:rebind 的第二个参数 |

变量引用检测：所有 `Variable` tag 的节点，除非是 `define`、`lambda` 参数列表、`let` 绑定中的名字。

### 4.1 Use-Site Collection（变量引用收集）

```
collect_uses(flat, node_id, current_scope):
  v = flat.get(node_id)
  if v.tag == Variable:
    sym_name = pool.resolve(v.sym_id)
    if sym_name 不是本 scope 或上游 scope 的绑定符号:
      return  // 自由变量，跳过（或标记为外部引用）
    uses_.push_back(node_id)
    // 更新当前 scope 的 refs：找到或创建对 sym 的引用条目
    update_refs(current_scope, v.sym_id, uses_.size() - 1)

  for each child in v.children:
    collect_uses(flat, child, current_scope)
```

### 4.2 Sym → All Scopes 索引（跨 scope 跳跃）

构建 `sym_scopes_` 表：遍历所有 scope，对每个 scope 的所有 def 和 ref 记录一条 `SymScopeRef`。然后按 sym_id 排序，生成 `sym_to_scopes_`。

查询 `query:def-use(x)`：
```
sym_id = intern("x")
range = sym_to_scopes_[sym_id]
如果 range.count == 0 → x 未定义（或未引用）
遍历 sym_scopes_[range.start .. range.start + range.count]:
  每个 SymScopeRef:
    is_def → def_nodes_[local_index] 是定义点
    !is_def → refs_[local_index] 是引用条目，通过 use_start/use_count 找到 use 点
```

## 5. 增量更新

### 5.1 脏标记传播

```
on_mutate(node_id):
  scope = find_scope_of_node(node_id)
  mark_dirty(scope)
  // 冒泡向上标记祖先 scope 的 dirty
  while scope.parent != INVALID:
    scope = &scopes_[scope.parent]
    scope->dirty = true
  // 如果 mutate 影响了符号绑定（set-body / rebind）
  // 还要标记 affected scope 为 dirty
```

### 5.2 选择性重建

```
rebuild_if_needed():
  if !built_:
    full_rebuild()
    return
  
  for each scope in scopes_ where dirty:
    rebuilt_count++
    // 清除该 scope 及其子 scope 的 def/use 数据
    clear_scope_data(scope)
    // 重新 extract
    extract_scope(flat, scope.node, scope.parent, 0)
    scope.dirty = false
    // 该 scope 的 use 数据在平铺向量中可能移动了
    // 需要 fixup 后续 scope 的偏移量
  
  // 如果有任何脏 scope 涉及符号定义变更
  // 也需要重建 sym_scopes_ 表中受影响的部分
  if syms_changed:
    rebuild_sym_scopes_for(dirty_sym_ids)

  generation_++
```

### 5.3 墓碑标记（避免全量向量碎片化）

增量删除绑定会留下空洞。用墓碑标记：

```cpp
struct ScopeNode {
    // ...
    bool tombstoned : 1;  // 已被 mutate:remove-node 删除
};

// rebuild 时 compact：遍历 scopes_，跳过 tombstoned
// 然后把后续 scope 的 parent/child 索引 fixup
```

墓碑不会累积太多——`mutate:remove-node` 在 AST 编辑中相对低频。当 `tombstoned > scopes_.size()/4` 时触发一次 full compact。

## 6. 查询 API 映射

### `query:def-use(sym)` — 核心查询

```cpp
QueryResult query_def_use(SymId sym) {
    auto range = sym_to_scopes_[sym];
    std::vector<NodeId> defs;
    std::vector<NodeId> uses;
    for (auto i = range.start; i < range.start + range.count; i++) {
        auto& ref = sym_scopes_[i];
        if (ref.is_def) {
            defs.push_back(def_nodes_[ref.local_index]);
        } else {
            auto& r = refs_[ref.local_index];
            for (auto j = r.use_start; j < r.use_start + r.use_count; j++) {
                uses.push_back(uses_[j]);
            }
        }
    }
    return {defs, uses};
}
```

### `query:reaches(node_id)` — 数据流下游

```cpp
// 给定一个 AST 节点，它定义了什么符号，这些符号被谁使用
QueryResult query_reaches(NodeId node) {
    ScopeNode* scope = find_scope_of_node(node);
    // 扫描 scope 的 defs，找到匹配 node 的 def
    for (auto i = scope->def_first; i < scope->def_first + scope->def_count; i++) {
        if (def_nodes_[i] == node) {
            return query_def_use(def_syms_[i]);
        }
    }
    return {};
}
```

### `query:effects(mutation)` — 影响范围

```cpp
// 给定一个 mutation 的描述，返回受影响的节点列表
// 需要 mutation 的参数（改哪个函数/哪种类型）
QueryResult query_effects(MutationSpec spec) {
    switch spec.kind:
        ReplaceFunctionBody:  // mutate:set-body fn
            // fn 的 def 点 → 所有 caller
            defs = query_def_use(spec.fn_name).defs
            for each def in defs:
                callers = find_callers(def)  // 调用此定义的 call 节点
                uses.extend(callers)
            return {defs, uses}
        
        ReplaceType:  // mutate:replace-type Int→Int64
            // 所有使用当前 type 的变量
            syms = find_all_syms_of_type(spec.old_type)
            for each sym in syms:
                result.uses.extend(query_def_use(sym).uses)
            return result
}
```

## 7. 性能模型

### 7.1 构建开销

假设 10K 节点 AST，~200 scope，~3000 个 use 点：

| 阶段 | 操作 | 时间估计 |
|:-----|:-----|:--------:|
| Scope tree | 一次 AST 全遍历 | ~50μs |
| Use collection | 每个 Variable 节点一次 push | ~30μs |
| Sym→Scope 排序 | 200 个 scope × ~5 sym = 1000 条目排序 | ~10μs |
| **全量构建** | | **~100μs** |
| 增量（scope local） | 单个 scope 重建（10 个绑定、100 个 use） | ~5μs |

全量构建比一次 `set-code` 解析慢一个数量级，但可接受。增量构建是纳秒级。

### 7.2 查询开销

| 查询 | 操作 | 时间估计 |
|:-----|:-----|:--------:|
| `query:def-use(n)` (n is local) | 1 次 sym→scope 跳转 + ~5 个 use 点 | ~100ns |
| `query:def-use(fn)` (fn is global) | 跨 scope 表定位 + ~20 个 use 点 | ~200ns |
| `query:reaches(node)` | 1 次 scope 内 def scan + 1 次 def-use | ~300ns |
| `query:effects(set-body fn)` | 1 次 def-use + 反向 caller 查找 | ~1μs |

全部数据在 L1/L2 cache 内（`scopes_` ~3KB, `uses_` ~24KB for 3K use points）。

### 7.3 SIMD 优化（远期）

批量替换场景（`mutate:map type Int→Int64`）：

```cpp
// 遍历所有 def_syms_，找到类型匹配的
// 用 SIMD 跳过不匹配的 symbol id
constexpr auto simd_width = 256 / 32;  // 8 x int32 per 256-bit
for (i = 0; i < def_syms_.size(); i += simd_width) {
    auto vals = _mm256_load_si256((__m256i*)&def_syms_[i]);
    auto cmp = _mm256_cmpeq_epi32(target, vals);
    auto mask = _mm256_movemask_epi8(cmp);
    while (mask) {
        auto bit = __builtin_ctz(mask);
        auto idx = i + bit / 4;
        // idx 是匹配的 def，查类型注释，匹配则加入结果
        mask &= mask - 1;
    }
}
```

单次 SIMD batch query：1M 个 def 点 ~ 5μs（纯 scan，无分支预测惩罚）。

## 8. 实现路标

### P0 — 简单全量 scan（1-2 天）

不缓存，每次查询遍历全部 AST。

```cpp
// query:def-use(x) → 临时 scan 整个 flat
std::vector<NodeId> simple_def_use(FlatAST& flat, SymId sym) {
    std::vector<NodeId> defs, uses;
    // 检测 lambda/let/define 参数列表
    // 收集所有 Variable 节点
    // 按 lexical scope 规则匹配
    return result;  // 慢但正确
}
```

**适用场景**：< 1K 节点、非频繁调用的原型。直接挂到 `query_impl.cpp` 的 `QueryEngine::match()`。

### P1 — Scope-level 缓存（3-5 天）

如本设计文档描述，做 scope tree + flat arenas + dirty 标记 + 增量重建。

**实现计划**：

```
Day 1: ScopeNode 定义 + extract_scope() 遍历
Day 2: Use collection + refs_ 索引
Day 3: Sym→Scope 跨 scope 跳跃表 + query:def-use 实现
Day 4: 增量重建（脏标记 + scope-local rebuild）
Day 5: query:reaches + query:effects 实现 + 测试
```

### P2 — Cross-scope + 性能优化（2-3 天）

```
Day 1: 闭包 capture 的跨 scope 处理
Day 2: SIMD batch query (for mutate:map)
Day 3: 并发 epoch（可选）
```

## 9. 集成到现有 QueryEngine

当前 `QueryEngine` 在 `query_impl.cpp` 中有 `match()` 和 `execute()`。def-use 查询是**新 QueryExpr::Kind**：

```cpp
enum class Kind {
    // ... 现有
    DefUse,          // (query:def-use "x")
    Reaches,         // (query:reaches 42)  // 按 NodeId
    Effects,         // (query:effects (mutate:set-body "render-loop"))
};
```

在 `QueryEngine` 上添加新成员：

```cpp
class QueryEngine {
    // 现有
    QueryExpr parse(std::string_view);
    bool match(NodeId, const QueryExpr&, int depth);
    std::vector<NodeId> execute(const QueryExpr&);
    
    // 新增
    void set_workspace(FlatAST* flat, StringPool* pool);
    DefUseIndex& ensure_defuse();
    std::vector<NodeId> query_def_use(SymId sym);
    std::vector<NodeId> query_reaches(NodeId node);
    
private:
    std::unique_ptr<DefUseIndex> defuse_;  // 懒初始化
};
```

`set-code` / `mutate:*` 后通过 `mark_dirty()` 通知 defuse 索引，在下次 `query_def_use()` 调用时增量重建。

## 10. 开放问题

1. **宏展开后的 def-use**：宏展开后会产生新符号绑定，但这些绑定在源码中不可见。建议：在 macro expansion pass 中标记 expanded node，def-use 对这些节点特殊处理（从 expanded body 中提取绑定）。

2. **mutate:rebind 的符号重建**：完全替换函数定义时，旧函数的 def-use 需要被清除。当前设计依赖脏标记 + rebuild，但 `rebind` 替换后的新函数体有全新的 scope tree。建议 mutate:rebind 后在旧 scope 上设 tombstone，为新函数建新 scope。

3. **自由变量的处理**：闭包引用的外部变量（自由变量）在当前设计中被跳过。长远来看需要 `query:captures` 追踪闭包的捕获集——这是闭包分析的扩展，不在 P0/P1 范围内。

4. **IR 侧的 def-use**：当前只在 tree-walker 层做（因为 eval 直接操作 FlatAST）。IR 有 flat instruction + symbol table，未来可以复用同一套 SymId 做 IR-level def-use（但这需要 IR pass manager 的增强——独立的 IR 设计文档）。
