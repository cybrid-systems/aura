# FlatAST → Source 反序列化设计

**版本**: v1.0
**状态**: 设计阶段
**对应**: M3e 工具链 — AST → Source

---

## 1. 问题

当前 Aura 的管线是单向的：

```
Source ──parse──→ FlatAST ──query-and-fix──→ mutated FlatAST
                    │                              │
                    ├── lower_to_ir ──→ execute     │
                    └── type_check  ──→ diag        │
                                                    │
                                              ⚠️ 写不回 source
```

Agent 可以修改 AST，但修改结果无法持久化到源代码文件。这切断了变异循环的最后一环。

## 2. 目标

```
Source ──parse──→ FlatAST ──→ mutate ──→ unparse ──→ mutated source
                    │                              │
                    │                              └── 写回文件 / 输出到 stdout
                    │
                    └── exec / typecheck / cache / ...
```

## 3. 设计

### 3.1 核心函数

```cpp
// 在 lowering_flat_impl.cpp 或新的 unparse.ixx 中

// 将 FlatAST 节点反序列化为 S-表达式字符串
// pool: 用于将 SymId 解析回字符串名
// indent: 当前缩进级别 (0 = 顶层)
std::string unparse_node(const FlatAST& flat, 
                          const StringPool& pool,
                          NodeId id, 
                          int indent = 0);
```

### 3.2 `NodeTag` → 输出格式

| NodeTag | S-表达式格式 | 示例 |
|---------|-------------|------|
| LiteralInt | 数字 | `42` |
| LiteralString | 引号字符串 | `"hello"` |
| Variable | 标识符 | `x`, `+`, `string-append` |
| Call | (函数 参数...) | `(+ 1 2)` |
| IfExpr | (if 条件 then else) | `(if (> x 0) x (- x))` |
| Lambda | (lambda (参数...) 体) | `(lambda (x y) (+ x y))` |
| Let | (let ((名 值)) 体) | `(let ((x 10)) x)` |
| LetRec | (letrec ((名 值)) 体) | `(letrec ((fact ...)) (fact 5))` |
| Define | (define 名 值) | `(define add (lambda (x y) (+ x y)))` |
| Begin | (begin expr...) | `(begin (display "a") (display "b"))` |
| Set | (set! 名 值) | `(set! x 42)` |
| Quote | (quote expr) / 'expr | `(quote (1 2 3))` |
| TypeAnnotation | (the 类型 expr) | `(the Int 42)` |
| Coercion | (coerce 类型 expr) | `(coerce String 42)` |

### 3.3 缩进策略

短表达式（< 60 字符）一行输出：

```
(+ 1 2)
(lambda (x) (* x 2))
```

长表达式换行缩进：

```
(letrec ((fact
           (lambda (n)
             (if (= n 0)
                 1
                 (* n (fact (- n 1)))))))
  (fact 5))
```

规则：
- 如果 `unparse` 结果 > 60 字符，在每个参数后换行
- 子表达式缩进 2 格
- Call 的函数名和第一个参数之间不换行

### 3.4 字符串转义

LiteralString 需要正确处理转义：

```cpp
std::string escape_string(std::string_view s) {
    std::string out;
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;
        }
    }
    out += '"';
    return out;
}
```

### 3.5 与 CompilerService 集成

```cpp
// 在 CompilerService 中新增

// 将上次解析的 AST 反序列化为 source
// 如果传入了 --query-and-fix, 反序列化变换后的 AST
std::string unparse_last() {
    if (!last_flat_) return "";
    return unparse_node(*last_flat_, last_pool_, last_flat_->root, 0);
}

// 反序列化指定的 FlatAST
std::string unparse(FlatAST& flat, StringPool& pool, NodeId root) {
    return unparse_node(flat, pool, root, 0);
}
```

### 3.6 CLI 接口

```bash
# 查看 AST 的 source 表示
echo '(+ 1 2)' | ./aura --unparse              # → "(+ 1 2)"

# 变换后再反序列化
echo '(+ 1 2)' | ./aura --query-and-fix '(node-type LiteralInt)' '(LiteralInt 99)' --unparse  
# → "(+ 99 99)"

# 多步管线
echo '(+ 1 2)' | ./aura --query-and-fix ... --unparse --write output.aura
```

### 3.7 与缓存 / CaaS 集成

```
define cache 扩展:
  ir_cache_[name] = {
      func:    IRFunction,
      source:  "原始 source",     // (已存在)
      ast:     FlatAST subgraph,  // (新增 — 变换后可重新 unparse)
  }
```

当 agent 通过 `--serve exec` 执行代码时：
1. 解析到 FlatAST
2. 检测到 define → 缓存 IR + ast + source
3. 如果 agent 请求 unparse → 从缓存的 ast 反序列化
4. 如果 AST 被变换过 → unparse 变变换后的代码

---

## 4. 实现计划

### Phase 1: 核心 unparser（~100 行）

在 `lowering_flat_impl.cpp` 中新增 `unparse_node()` 函数：
- 处理所有 NodeTag
- 递归 walk children
- 缩进策略
- 字符串转义

### Phase 2: CLI 集成（~20 行）

在 `main.cpp` 中新增：
- `--unparse` 标志：解析输入 → unparse → 打印
- 与 `--query-and-fix` 组合使用

### Phase 3: CompilerService 集成（~30 行）

在 `service.ixx` 中：
- 存储 last FlatAST + StringPool
- 提供 `unparse()` 方法
- 通过 --serve `{"cmd":"unparse"}` 返回 source

### Phase 4: 文件写回（~20 行）

```bash
echo '(+ 1 2)' | ./aura --query-and-fix ... --write output.aura
```

---

## 5. 与增量编译的关系

```
增量编译管线 + unparse:

                  ┌──→ define cache ──→ hot-swap
                  │       │
source ──parse──┼──→ exec ──→ result
                  │       │
                  └──→ query-and-fix ──→ mutated AST
                                             │
                                     ┌───────┼────────┐
                                     │       │        │
                                     ▼       ▼        ▼
                                  exec    unparse   cache
                                 验证    写回文件   更新 source
```

核心闭环：
1. agent 定义函数 → 缓存 source + AST + IR
2. agent 变换 AST → 验证正确性
3. unparse → 写回文件
4. 再次读取时从文件 parse → cache 命中 → 增量编译

---

## 6. 代码结构

```cpp
// 建议位置: src/compiler/unparse_impl.cpp (新增文件)
// 导出: src/compiler/lowering.ixx (复用现有导出模块)

namespace aura::compiler {

std::string unparse_node(const FlatAST& flat,
                          const StringPool& pool,
                          NodeId id,
                          int indent = 0) {
    if (id == NULL_NODE || id >= flat.size()) return "()";
    auto v = flat.get(id);
    
    switch (v.tag) {
    case NodeTag::LiteralInt:
        return std::to_string(v.int_value);
    
    case NodeTag::LiteralString:
        return escape_string(pool.resolve(v.sym_id));
    
    case NodeTag::Variable:
        return std::string(pool.resolve(v.sym_id));
    
    case NodeTag::Call: {
        std::string s = "(";
        s += unparse_node(flat, pool, v.child(0), indent);
        bool multiline = false;
        // ... accumulate args, check length for multiline ...
        s += ")";
        return s;
    }
    
    // ... handle each NodeTag ...
    }
}

} // namespace aura::compiler
```

---

## 7. 测试

```
Input                     → Expected Output
────────────────────────────────────────────
42                        → 42
(+ 1 2)                   → (+ 1 2)
(lambda (x) (* x 2))      → (lambda (x) (* x 2))
(let ((x 10)) x)          → (let ((x 10)) x)
"hello"                   → "hello"
(if (> x 0) x (- x))     → (if (> x 0) x (- x))
(define add (lambda (x y) (+ x y)))  → (define add (lambda (x y) (+ x y)))
```

所有测试通过 `echo 'expr' | ./aura --unparse` 验证。
