# Aura C++ 后端代码审查 — 2026-05-13

**来源**: 外部代码审查（as of May 2026 commits）
**覆盖**: C++26 backend architecture, FlatAST, PassManager, QueryEngine, ABF

---

## 总体评价

Ambitious, early-stage AI-native Lisp-like language with strong emphasis on
auto-mutating ASTs, queries/transforms for AI agents, and incremental compilation.
The C++ backend already shows thoughtful architecture for an interpreter/compiler
hybrid. Modern C++ (C++26 modules, concepts, pmr arenas, SoA/DOD, functional-style
passes), no heavy OO, high-performance focus.

---

## 优势（基础上佳）

| 维度 | 评价 |
|------|------|
| Modern C++ | C++26 modules, pmr arenas, span, expected, concepts, constexpr tables, P2996 反射 |
| DOD | FlatAST SoA（独立 vector 存 tag/int/sym_id/child），零拷贝查询，缓存友好 |
| 函数式管线 | Pass concept + run_pipeline fold，纯函数风格 |
| AI/Compiler 协同 | FlatAST + Patch + Query/Transform/AutoFix + HotSwap |
| 测试/CLI | 48 CTest, --serve JSON, --query/--ir, Racket #lang prototype |

---

## 改进建议

### 1. AST & Data Layout（强化 DOD）

当前 FlatAST SoA 架构良好，可进一步：

```cpp
// 建议：添加 nodes() view 到 FlatAST
auto nodes() const {
    return std::views::iota(0u, size())
         | std::views::transform([this](NodeId id) { return get(id); });
}
```

- **std::mdspan**：对 child/param 数据使用多维视图（C++23/26 可用时）
- **Consteval 扩展**：扩大 kNodeMeta 表，添加 consteval 节点构造器/校验器
- **反射集成**：用 P2996 自动生成序列化、dispatch 表（已有初步：TagDispatch → table）
- **Span + Ranges**：解析器/降级器全面使用 std::span，查询用 std::views::filter 替代手写循环

### 2. PassManager & Pipeline（更函数式）

- Pass 纯函数化：返回新 IRModule 或使用不可变视图 + patch
- 添加 std::execution policy 或 SIMD 用于批量 pass（如常量折叠跨基本块）
- 增加更多 `[[expects]]` / `[[ensures]]` 契约

### 3. 已知进展

这些建议中部分已在当前代码中着手：
- `kNodeMeta` 已是 constexpr 表（16 节点）
- 查询引擎已用 `std::views::filter`（`calls_to`, `refs_of`）
- TagIndex 已实现 O(1) by_tag（替代 O(N) switch）
- ABF dispatch 表已从 switch 迁移到 static registry
- Pass concept + fold pipeline 已实现（`run_pipeline<CK, Arity, CF>`）
- Contracts 已启用（`-fcontracts`），在 arena 边界使用

### 4. 后续方向

- FlatAST `nodes()` view 快捷方法
- 更多 contract annotation（[[expects]] / [[ensures]]）
- 逐步减少手写 span → std::views
