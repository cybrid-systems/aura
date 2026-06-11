# C4: 编译期 AST 验证 — 设计文档

**目标**：Ghuloum Step 17 — 在宏展开后、求值前，对 AST 做结构验证。早期发现错误，提供更清晰的位置和原因报告。

---

## 现状

当前错误发现靠运行时：
- `eval_flat` 遇到非法结构时返回 `Diagnostic`
- 类型检查在 `--strict` 模式下前置运行
- 宏展开后没有结构验证步骤

```
macro_expand_all → eval_flat       (错误在运行时发现)
                 → typecheck       (仅类型，不检查结构)
```

---

## 设计

### 核心思路：`ast_validate` pass

在 `macro_expand_all` 之后、`needs_tree_walker_fallback` 之前，插入 AST 验证步骤：

```
macro_expand_all → ast_validate → needs_tree_walker? → eval (IR or TW)
                                  ↑
                           新 pass，返回错误列表
```

### 验证规则

| # | 规则 | 检查 | 示例错误 |
|---|------|------|----------|
| V1 | `if` 参数数 | `(if c t e)` 必须恰好 3 个子节点 | `(if x) → if requires 3 args, got 1` |
| V2 | `let` 结构 | `let` 必须有 2 子节点 (value, body) | `(let ((x 1))) → let requires value and body` |
| V3 | `lambda` body | `lambda` 必须有 body | `(lambda (x)) → lambda requires body` |
| V4 | `define` 结构 | `define` 必须有 value | `(define x) → define requires value` |
| V5 | `set!` 结构 | `set!` 必须有 target 和 value | `(set! x) → set! requires target and value` |
| V6 | `cond` 子句 | `cond` 至少需要一个子句 | `(cond) → cond requires at least one clause` |
| V7 | `case` 结构 | `case` 必须有至少一个 clause | `(case x) → case requires at least one clause` |
| V8 | 引用完整性 | 所有 child NodeId 在当前 FlatAST 范围内 | `invalid node reference` |

### 接口

```cpp
// ast_validate.ixx / ast_validate_impl.cpp
namespace aura::compiler {

struct ValidationError {
    SourceLocation location;
    std::string message;
    std::string suggestion;
};

struct ValidationResult {
    std::vector<ValidationError> errors;
    bool valid() const { return errors.empty(); }
};

// Validate a FlatAST for structural correctness.
// Returns list of errors; empty = valid.
ValidationResult validate_ast(const FlatAST& flat, const StringPool& pool,
                               NodeId root);

} // namespace aura::compiler
```

### 集成到 CompilerService

在 `eval()` 中：

```cpp
auto expanded_root = macro_expand_all(...);

// NEW: AST validation pass
{
    auto vr = validate_ast(*flat_ptr, *pool_ptr, expanded_root);
    if (!vr.valid()) {
        for (auto& e : vr.errors)
            std::println(std::cerr, "{}", e.message);
        // Non-fatal for now (warnings); in --strict becomes fatal
    }
}

if (needs_tree_walker_fallback(...)) { ... }
```

### 实现位置

新建文件：
- `src/compiler/ast_validate.ixx` — 模块声明
- `src/compiler/ast_validate_impl.cpp` — 实现

或直接放在 `service.ixx` 中作为 CompilerService 方法（减少文件改动）。

推荐直接放在 `service.ixx` 作为私有静态方法，简化构建依赖。

---

## 时间估算

| 步骤 | 估计 |
|------|------|
| 实现 validate_ast (V1-V5) | 1h |
| V6-V8 | 0.5h |
| 集成到 eval() | 0.5h |
| 测试 | 1h |
| **总计** | **3h** |

---

## 验收标准

```scheme
(if 1)             → warning: if requires 3 args, got 1
(define x)         → warning: define requires value
(lambda ())        → warning: lambda requires body
(let ((x 1)))      → warning: let requires body
(set! x)           → warning: set! requires target and value
```

不影响正确表达式的求值：

```scheme
(if #t 1 2)        → 1  ✅ (无 warning)
(define (f x) x)   → ✅ (无 warning)
```
