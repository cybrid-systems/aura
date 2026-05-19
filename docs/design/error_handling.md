# Aura 错误处理设计 v2

> 从 C++ 端的 `Result<T>` 到 Aura 层的 try/catch 闭环。

---

## 现状

```
C++ 端: EvalResult = std::expected<EvalValue, Diagnostic>
        已有 ErrorKind enum + SourceLocation + context chain

Aura 端: (error "msg") → throw std::runtime_error → 进程崩
         (assert expr)  → 同上
```

Aura 层没有任何捕获能力——`(error "x")` 直接抛 C++ 异常，不可恢复。

---

## 设计

### 1. 异常类型体系 (Aura)

```scheme
; 三种内置异常类型:
<error>              ; 通用错误（基类）
  <error:parse>      ; 解析错误
  <error:type>       ; 类型错误
  <error:runtime>    ; 运行时错误
    <error:arity>    ; 参数数量不匹配
    <error:bound>    ; 未绑定变量
    <error:assert>   ; 断言失败
    <error:io>       ; 文件 I/O 错误
```

类型用现有的 type system 的变体类型实现，或简单的 tag 字符串。

### 2. (error expr) — 抛异常

```scheme
(error "something went wrong")        ; 抛出通用错误
(error <error:runtime> "bad value")    ; 带类型标签
(error <error:assert> "x should be positive")

; 实现: 设置一个 EvalValue 变体为 Error 类型
; 在 eval_flat 中检查: 如果子表达式返回 Error，停止传播
```

### 3. (try body catch handler) — 捕获

```scheme
(try
  (risky-operation)
  (catch (e)                    ; e 绑定到错误值
    (display "caught: ") (display e) (newline)
    42))                        ; 返回默认值

; 带类型过滤:
(try
  (/ 1 0)
  (catch (<error:runtime> e)    ; 只捕获运行时错误
    "division by zero"))

; catch-all:
(try
  (error "oops")
  (catch _ "default"))          ; _ 匹配所有错误

; 无错误时:
(try 42 (catch _ 0)) → 42
```

### 4. (raise expr) — 重新抛出

```scheme
(try
  (try
    (error "inner")
    (catch (e)
      (display e)               ; 打印后重新抛出
      (raise e)))
  (catch (e) "caught outer"))   ; 外层还能捕获
```

### 5. (guard expr clause ...) — 带条件的错误处理

```scheme
(guard (val <- (risky-call))
  (else (e) (display "failed: ") (display e) (newline) 0))
```

### 6. 实现方案

```
C++ 端:
  EvalValue 新增一个 Error 变体 (variant index)

  eval_flat 中:
    for each child expr:
      result = eval_flat(child)
      if (is_error(result)):
        if (parent is try/catch): 跳转到 catch handler
        else: 向上传播 error
  
  try/catch 作为特殊形式 (不是原语):
    (try body catch-handler)
    → eval_flat 识别 Try 节点
    → eval body
    → 如果 body 返回 Error，用 catch handler 的 eval result 替代
```

### 7. FlatAST 节点

```cpp
NodeTag::Try = 0x12  // 新增: (try body catch-pattern handler)
NodeTag::Raise = 0x13  // 新增: (raise expr)
NodeTag::Guard = 0x14  // 新增: (guard pattern <- expr clause...)
```

Try 节点结构:
- child[0] = body expression
- child[1] = catch pattern (Variable "_" 或 error type tag)
- child[2] = handler expression
- sym_id = catch variable name (或 INVALID_SYM)

### 8. 迁移路径

```
Phase 1: C++ eval_flat 支持 Error 变体 + Try 特殊形式  ← 现在
Phase 2: Aura (error), (try), (raise) 原语/语法
Phase 3: 所有内建函数改抛 Error 而非 throw/crash
Phase 4: 类型系统识别 Error 类型（可选）
```
