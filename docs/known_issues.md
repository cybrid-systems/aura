# Aura 已知问题

更新：2026-05-19 收盘 — 今日 10 个提交后

---

## 已无 P0 阻塞性 Bug

### ~~1. 🔴 缓存函数字符串分支崩溃~~ ✅ `3e3e7a2`

**根因**：`Arg` 执行将负整数误判为 cell sentinel 索引。`-1` 被解码为 `cell_slot=0` 指向无关的局部变量。这是真正的根因，不是 ConstString heap 的问题。
**修复**：仅在解码后的 cell slot 在 `cell_heap_` 范围内时才视为 cell 引用。

### ~~2. 🔴 `set!` 闭包可变状态不持久~~ ✅ `3392d77`

**根因**：`let` 用 `bind(name, Int(0))` 直接存值，`set!` 通过 `lookup_cell_ptr` 找不到 Cell 就静默跳过。
**修复**：`let` 现在像 `letrec` 一样用 Cell 绑定，`lookup()` 自动解 Cell，`lookup_cell_ptr` 找到 Cell 后 `set!` 正常工作。

---

## P1 — 严重

### 3. 缓存函数自引用

**症状**：`cache_define` 时 `ir_cache_` 为空，函数 body 中的自引用 Variable 降级为 `ConstI64 0`。
```scheme
(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))
(fact 5)   → 120  ✅ (走 tree-walker，正确)
(fact 5) --ir → 0  ❌ (缓存 IR 中有毁坏的 self-call)
```
**当前缓解**：`e334194` — 缓存函数名加入 `user_bindings_`，触发 tree-walker fallback，确保正确性。
**根治方向**：`cache_define` lowering 时对自引用 Variable 做特殊处理（如预占 func_id），使 IR 路径也正确。

### ~~4. `cadr`/`caddr` 已存在~~ ✅

`cadr`、`caddr`、`cddr` 等 CxR 组合器是**内建原语**（`register_primitive` 注册），LLM 测试失败是其他原因导致。

### 5. 缓存函数内嵌 lambda

~~**症状**：跨表达式定义并调用含 `foldl` + 内嵌 lambda 的缓存函数时结果不正确。~~

**修复**：`3cb9a33` — `cache_module`/`cache_define` 检查函数体中的变量引用是否可 lowering。自递归跳过 IR 缓存。

---

## P2 — 功能缺口

### 6. LLM Agent 多行输入兼容

**症状**：LLM 输出的 EDSL 代码含 `(set-code "...\n...")` 等多行内容，JSON `\n` 转义曾在自制解析器中未处理。目前已修复。
**状态**：✅ `e9b4bbf` — JSON 解析器支持 `\n`、`\"`、`\\`、`\t` 等转义序列。

### 7. arity 检查在 eval() 路径中恢复

**状态**：✅ `07c196d` — 恢复启用 + 添加警告输出。原 false positive 问题已被后续修复解决。
```scheme
(+ 1 2 3)   → 6  ✅ (variadic arity 正确)
(define (f x) x) (f 1 2)  → arity warning (非致命)
```

### 8. 桥接器 body_source fallback 未完全验证

`02681ac` — `body_source` 已加入 `ClosureBridgeData`，但 fallback 路径缺少测试覆盖。

### 9. 标准库导出

所有 18 个标准库文件 export 列表完整。`106/106` bash 测试通过。

---

## P3 — 增强

### 10. 深递归

```scheme
(define (deep n) (if (= n 0) 0 (deep (- n 1))))
(deep 600)  → error: recursion depth exceeded (>400)
```
**状态**：从 segfault 改为友好错误 ✅ (`85c3815`)。root fix（显式调用栈）预留为独立里程碑。

### 11. stdout 不 flush

`(display "a") (display "b")` 在 pipe 模式下连在一起输出。`newline` 可以 flush。

### 12. 类型检查未完全增量

`typecheck-current` 当前全量遍历。

---

## ✅ 今日已修复（5/19 完整清单）

| 问题 | 提交 |
|------|------|
| 🔴 Arg 负整数 cell-ref 崩溃 | `3e3e7a2` |
| 🔴 `set!` 闭包可变状态 | `3392d77` |
| `hash-has-key?` 原语 | `3392d77` |
| Arity 检查恢复 (P2#8) | `07c196d` |
| 深递归友好错误 (P3#11) | `85c3815` |
| 自引用缓存函数 (tree-walker fallback) | `e334194` |
| Agent prompt 更新 | `f1ebc13` |
| JSON 解析器 `\n` 转义 | `e9b4bbf` |
| EDSL `(current-source)` 原语 | `426e877` |
| 错误格式统一 (suggestion 字段) | `c8e8baf` |
| `wrong_arity` + `type_of` typecheck 修复 | `09e71f0` |
| try/catch IR 指令 | `6d06e67` |
| 标准库 v2 (iter/queue/stack/random) | `4b85e46` |
