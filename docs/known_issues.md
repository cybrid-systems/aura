# Aura 已知问题

更新：2026-05-18 — AI Agent 实测后发现

---

## P0 — 阻塞性

### 1. `cache_define` 复杂 Lambda 体不持久化

**症状**：`(define (f ...) ...hash-ref/hash-set! body...)` 后，`f` 在后续表达式中不可访问。

```
> (define (inner words) (let ((h (hash))) (foldl (lambda (acc word)
    (let ((cnt (hash-ref h word)))
      (if (void? cnt) (hash-set! h word 1)
          (hash-set! h word (+ cnt 1)))) acc) (list) words)))
> inner
error: unbound variable: inner
```

**可重现**：仅在 body 包含 hash-ref + hash-set! 组合时触发。简单 body（42, `(foldl + 0 ...)`）正常。

**原因推测**：`cache_define` 内 `eval_flat` 的 FlatAST arena 与顶层共享，复杂 body 的闭包捕获导致 arena 提前释放或 env 被覆盖。

**影响**：AI Agent 无法定义任何有用函数。**阻塞 AI 管线闭环**。

### 2. `require` 不能放在函数体内部

**症状**：LLM 生成的代码试图在函数体内放 `require`，解析错误。

```
(define (word-count filename)
  (require std/hash all:)        ← 非法
  ...)
```

**原因**：Aura 的 `require` 只允许在顶层，内嵌在 define/lambda/let 体内会解析失败。

**影响**：LLM 经常生成这种代码，导致 Phase 1 失败后进入 EDSL 修复死循环。

**临时方案**：在 system prompt 中明确禁止。

### 3. Agent 脚本遇到 LLM 空响应时死循环

**症状**：当 LLM 返回不含代码块的纯文本时，agent 不知道结束，持续重试。

**原因**：`extract_code` 返回空列表时，agent 把纯文本作为 assistant message 继续发送，不检查 DONE 状态。

**影响**：浪费 API 调用次数。

---

## P1 — 严重

### 4. Parser 错误恢复有限

**症状**：当前实现跳过无效表达式继续解析，但 pipe 模式的表达式分割器（main.cpp）在传递给 parser 之前就检查了括号平衡，所以 parser 的恢复机制对 pipe 模式影响有限。

### 5. `set-code` 内容截断

**症状**：LLM 生成的代码超过 ~4000 tokens 时被截断，`set-code` 收到不完整的代码，产生模糊错误。

**影响**：EDSL 修复循环中代码越来越大，最终必然截断。

**当前缓解**：auto-retry 将 max_tokens 翻倍到 16000，但 LLM 仍可能输出截断。

### 6. 错误信息不够详细

**症状**：`"unbound variable: n"` 之类的错误不让 LLM 明白根因（`n` 从哪里来？是变量名的一部分还是宏展开的临时名？）。

**影响**：LLM 无法从错误信息推断修复方向，陷入猜测循环。

---

## P2 — 功能缺口

### 7. 缺少 `for-each` 原语

**症状**：LLM 不断生成 `for-each`，需要 prompt 多次阻止。

**当前替代**：`foldl`。

### 8. 缺少 `string-split-words` / 标准库补全

**症状**：LLM 尝试调用 `string-split-words` 但 import 前缀机制导致找不到。

**状态**：函数实际存在于 `std/string.aura`，但使用了 `map` 等未导出的函数。

### 9. 缺少标准排序函数

**症状**：LLM 生成 `sort` 但不存在。

---

## P3 — 增强

### 10. `string->list` / `list->string` 对链式操作不够直观

**症状**：LLM 经常写出 `(list->string (map ... (string->list s)))` 但 `map` 不可用。
