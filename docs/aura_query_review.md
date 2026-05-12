# AuraQuery Engine — 性能审查报告

**版本**：v1.0
**审查日期**：2026-05-12
**覆盖范围**：query.ixx (438 行) + query_impl.cpp (122 行)

---

## 1. 架构现状

```
Query S-表达式
    │
    ▼
tokenize() → PS → parse_expr() → QueryExpr AST
    │
    ▼
execute() → for (NodeId id : 0..N-1) match(id, QueryExpr)
    │                              │
    │                  ┌───────────┴───────────┐
    │                  ▼                      ▼
    │             ASTIndex               FlatAST SoA
    │            (views/filters)         (9 pmr::vectors)
    │
    ▼
vector<NodeId> 结果集
```

**核心路径**: `execute()` 对 FlatAST 做 **O(N) 全量线性扫描**，每节点调用 `match()` 递归匹配。

---

## 2. 瓶颈分析

### 🔴 P0: 全量扫描 (Critical)

```cpp
std::vector<NodeId> execute(const QueryExpr& q) {
    std::vector<NodeId> r;
    for (NodeId i = 0; i < index_.ast.size(); ++i)  // ← 100% 扫描
        if (match(i, q)) r.push_back(i);
    return r;
}
```

**问题**：无论查询多简单（例如 `(node-type Call)`），都扫描整个 FlatAST。在 AI Agent 场景下，AST 可能包含 10⁵-10⁶ 个节点。

**影响**：`q("Call")` 即使有 1000 个匹配，也需要扫描 100000 个节点。

### 🔴 P1: 无标签索引 (Critical)

```cpp
// ASTIndex::by_tag 是纯 filter view，每次都从头过滤
auto by_tag(NodeTag t) const {
    return views::iota(0u, ast.size())
         | views::filter([this, t](NodeId id) {
               return ast.get(id).tag == t;  // ← 每次 O(N)
           });
}
```

`ASTIndex` 不维护标签索引。`by_tag()` 返回的 range 每次迭代都做标签检查。对比：

```
当前: by_tag(Call) → O(N), 逐个比对 tag
优化: tag_index[Call] → O(1), 直接返回预编译的节点列表
```

### 🟡 P2: SymRefIndex 与 QueryEngine 解耦

```cpp
// SymRefIndex 有倒排索引，但 QueryEngine 不用它
// QueryEngine 的 calls_to() / refs_of() 自己做线性扫描
```

`SymRefIndex::build()` 已经扫描过一次并建立了 `SymId → vector<NodeId>` 的倒排索引。但 `QueryEngine::match(Callee)` 不用这个索引——它自己做 `ast.get(v.child(0)).sym_id == sym` 的线性判断。这两套索引应该合并。

### 🟡 P3: pool.intern() 在匹配热路径中

```cpp
// ASTIndex::calls_to(name) 和 refs_of(name) 每次都 intern
auto sym = pool.intern(name);
```

`intern()` 是字符串哈希 + 可能的内存分配。虽然 `StringPool` 是开放寻址，但在查询热路径中每次调用是不必要的——查询时只需要符号名匹配，不需要产生新的 intern 记录。

### 🟢 P4: 递归 match() 无剪枝

```cpp
// Exists / HasChild 递归遍历所有子节点
case Exists:
    if (match(id, q.children[0])) return true;
    for (auto c : v.children) if (match(c, q)) return true;
```

深层嵌套的 AST（10+ 层 lambda/let 嵌套）会触发显著的递归开销。没有深度限制或迭代转换。

### 🟢 P5: 查询结果无缓存

每次 `execute()` 创建新的 `vector<NodeId>`。AutoFixEngine 里多个 `(query, fix)` 规则之间的中间结果不共享。

### 🟢 P6: Patch 生成效率

`generate_patches()` 对每个匹配节点调用 `build_node()` 重建替换模板。模板解析在 `parse_replace()` 中每次重新 tokenize。

---

## 3. 优化建议

### 3.1 标签索引 (TagIndex) — P0

为 FlatAST 的 11 种 NodeTag 预建索引：

```cpp
// 在 FlatAST 构造时或首次查询时构建
struct TagIndex {
    std::array<std::vector<NodeId>, 11> by_tag;
    
    void build(const FlatAST& ast) {
        for (NodeId i = 0; i < ast.size(); ++i)
            by_tag[static_cast<int>(ast.get(i).tag)].push_back(i);
    }
    
    std::span<const NodeId> nodes(NodeTag t) const {
        return by_tag[static_cast<int>(t)];
    }
};
```

**效果**：
```
q("Call")  之前  O(N)   之后  O(K)   (K = 匹配数)
q("If")    之前  O(N)   之后  O(K)
```

### 3.2 集成倒排索引 — P1

将 SymRefIndex 合并到 ASTIndex 中：

```cpp
// 对每个 SymId 预计算引用列表
// calls_to("+") → 直接查 SymId("+") 的引用列表中 tag=Call 的部分
```

### 3.3 惰性求值 + 流式结果 — P2

```cpp
// 返回 view 而非 vector，支持分页/流式
// 在 --serve 模式下可以增量推送结果给 Agent
auto execute(const QueryExpr& q) {
    return tag_index(q.node_tag)                    // 先过滤标签
         | views::filter([&](NodeId id) {           // 再精细匹配
               return match_quick(id, q);           // match_quick 做最短路径匹配
           });
}
```

### 3.4 模板 tokenize 缓存 — P3

```cpp
// TransformEngine 缓存解析后的 ReplaceTemplate
// 避免每次 query_and_fix 都重新 tokenize
std::unordered_map<std::string, ReplaceTemplate> template_cache_;
```

### 3.5 查询编译 (Query JIT) — Future

```cpp
// 将 QueryExpr 编译为一个轻量 match 函数
// 利用 P2996 反射生成类型特化的匹配代码
using CompiledQuery = bool(*)(NodeId, const FlatAST&);
CompiledQuery compile(const QueryExpr& q);  // → 生成专用匹配函数
```

---

## 4. 性能基线估算

假设：FlatAST 100,000 节点，查询 1000 次

| 优化 | 当前时间 | 优化后 | 加速比 |
|------|---------|--------|--------|
| TagIndex + SymIndex | 5-10 ms/query | 0.01-0.1 ms | ~100x |
| 查询缓存 | 50-100 ms/batch | 5-10 ms | ~10x |
| 模板缓存 | 0.5 ms/rule | 0.001 ms | ~500x |
| 流式结果 | 全量内存 | 分页 1KB | 内存 100x |

---

## 5. 实施建议

### 短期（1-2 天）
1. 在 `FlatAST` 构造时建 TagIndex（一行 `switch` 分发）
2. 将 `ASTIndex::by_tag()` 改为用 TagIndex 查询
3. 缓存 `parse_replace` 结果

### 中期（3-5 天）
4. 合并 SymRefIndex → ASTIndex（自动维护）
5. `execute()` 返回惰性 range 而非 vector
6. match 递归转迭代（深度限制）

### 长期（Future）
7. 查询结果复用（Multi-Query 共享中间结果）
8. 查询编译 (P2996 反射生成匹配函数)

---

> **核心发现**：当前查询引擎的逻辑正确但全量扫描(Q(N))
> 加入 TagIndex 一个改动就能把 `(node-type Call)` 从 O(N)→O(K)。 这是性价比最高的优化，大概 20 行代码，100 倍加速。
