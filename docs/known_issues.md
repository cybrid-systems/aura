# Aura 已知问题

更新：2026-05-18 v2 — 修复后的真实状态

---

## P0 — 阻塞性

### 1. ~~`cache_define` 复杂 Lambda 体不持久化~~ ✅ 已修复

**提交**: `af04a12` + `37f1361`

**根因**：Lambda lowering 计算的自由变量列表从未赋值给 `IRFunction.free_vars`，导致桥接器绑捕获变量为空（`func.free_vars = free_vars` 缺失）。同时 IR 和树遍历器的闭包 ID 命名空间冲突（均从 1 开始），`require` 加载 stdlib 后树遍历器占用了低 ID，后续 IR 闭包冲突导致 `apply_closure` 拿到错误的闭包。

**修复**：
- `func.free_vars = free_vars`（`lowering_impl.cpp`）
- `next_closure_id_` 起始值改为 `1ull << 48`（`ir_executor.ixx`）
- 缓存函数 bundle 的 `lambda_fid` 取第一个函数而非最后一个
- `remap_func_ids` 偏移量修正

### 2. `require` 在函数体内不支持（IR 路径）

**症状**：LLM 生成的代码试图在函数体内放 `require`：
```lisp
(define (word-count filename)
  (require std/hash)        ← IR 路径下失效
  ...)
```

**原因**：tree-walker 路径（`eval_flat` 的 Call 分支 special form handler）确实支持 `require` 在函数体内。但 `cache_define` = lowering→IR 时，`require` 不在 `prim_call_map` 中，被降级为 `ConstI64 0`。缓存函数被 IR 调用时 `require` 不执行。

**状态**：函数定义时会 fallback 到 tree-walker（因为 `needs_tree_walker_fallback` 检测到 `require`），所以 define 本身能过。但后续调用走 IR 缓存时出错。并非解析器问题，是 IR 缓存路径断裂。

**影响**：LLM 生成含 `require` 的函数时，定义成功但调用失败。

**临时方案**：system prompt 明确禁止；或把 `require` 放在顶层，函数体内只使用已导入的名称。

### 3. Agent 脚本遇到 LLM 空响应时死循环

**症状**：当 LLM 返回不含代码块的纯文本时，agent 不知道结束，持续重试。

**原因**：`extract_code` 返回空列表时，agent 把纯文本作为 assistant message 继续发送，不检查 DONE 状态。

**影响**：浪费 API 调用次数。

---

## P1 — 严重

### 4. 错误信息不够详细

**症状**：`"unbound variable: n"` 之类的错误不让 LLM 明白根因（`n` 从哪里来？是变量名的一部分还是宏展开的临时名？）。

**影响**：LLM 无法从错误信息推断修复方向，陷入猜测循环。

**当前状态**：已添加行号列号和猜测补全（`did you mean x?`），但对 LLM 消费仍然不够。

### 5. `set-code` 内容截断

**症状**：LLM 生成的代码超过输入缓冲区限制时被截断，`set-code` 收到不完整的代码，产生模糊错误。

**当前缓解**：auto-retry 将 max_tokens 翻倍到 16000。

### 6. IR 桥接器依赖 FlatAST 指针持久性

**症状**：缓存函数内部的内嵌 lambda 被 `foldl`/`map`/`filter` 等原语调用时，桥接器需要 FlatAST 和 StringPool 指针来 `eval_flat` lambda 体。这些指针保存自 `cache_define` 时的 arena 分配区，跨 eval() 调用后可能失效。

**当前状态**：主路径已修复（`cache_strings_` + 字符串池重映射）。但深层嵌套场景或 `unload_module` / `reset` 后暴露的问题尚未完全验证。

---

## P2 — 功能缺口

### 7. 缺少 `for-each` 原语

**症状**：LLM 不断生成 `for-each`，需要 prompt 多次阻止。

**当前替代**：`foldl`。

### 8. 标准库导出不完整

**症状**：LLM 尝试调用 `map`/`string-split-words`/`sort` 等但找不到。

**具体缺失**：
- `list.aura` 未导出 `map`（`map` 可能在 `combinators` 中或 primitives 层面存在，但 LLM 从 list 模块找不到）
- `string.aura` 的函数使用了未导出的依赖
- 无 `sort` 标准实现（`list.aura` 内有 pivot-based sort 但不够健壮）

### 9. `string->list` / `list->string` 链式操作不直观

**症状**：LLM 经常写出 `(list->string (map ... (string->list s)))` 但 `map` 不可用。

---

## P3 — 增强

### 10. IR 覆盖不全

**当前 fallback 项**：try/catch/raise、apply（primitive 但非 IR opcode）、when/unless、cond/case、宏定义。这些走 tree-walker，影响函数内联时的性能预测。

### 11. 类型检查未完全增量

**代码注释确认**：`typecheck-current` 当前全量遍历，增量跳过是 future work。

---

## ✅ 已修复（历史）

| 问题 | 提交 | 状态 |
|------|------|------|
| `cache_define` 复杂 lambda 不持久化 | `af04a12` + `37f1361` | ✅ |
| IR 闭包 ID 与树遍历器冲突 | `af04a12` | ✅ |
| 缓存函数内 display 输出丢失 | `37f1361` | ✅ |
| 缓存函数字符串池索引错位 | `37f1361` | ✅ |
| IR 桥接器 `lambda_fid` 指向最后一个而非第一个函数 | `37f1361` | ✅ |
| `remap_func_ids` 偏移量多加了 1 | `37f1361` | ✅ |
