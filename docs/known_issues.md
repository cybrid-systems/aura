# Aura 已知问题

更新：2026-05-19 — LLM 锤炼测试后

---

## P0 — 阻塞性

### 1. 🔴 缓存函数字符串分支崩溃

**症状**：缓存函数的 `if` 表达式取第二条字符串分支时崩溃（`std::bad_variant_access`）。
```scheme
(define (f n) (if (< n 0) "neg" "pos"))
(f 1)   → "pos"  ✅
(f -1)  → CRASH  ❌
```
**根因**：`ConstString` 每次执行都向 `primitives_.string_heap()` push。两个不同 `IRInterpreter` 实例共享同一个 prim heap。第二个分支的字符串索引错位，`std::get<wrong_type>` 崩溃。
**影响**：任何缓存函数中取第二条字符串分支都会崩溃。

### 2. 🔴 `set!` 闭包可变状态不持久

**症状**：闭包内的 `set!` 修改不跨调用持久。
```scheme
(define (make-counter)
  (let ((count 0))
    (lambda () (set! count (+ count 1)) count)))
(define c (make-counter))
(c) (c) (c)  → 全部返回 1（应为 1, 2, 3）
```
**根因**：`let` 的绑定用 `bind(name, value)` 直接存值。`set!` 通过 `lookup_cell_ptr` 查找 Cell，找不到就静默跳过。`letrec` 用 Cell 绑定所以工作，但 `let` 不用。
**修复方向**：需要让 `let` 在闭包创建时用 Cell 绑定，或者在 lowering 时检测被闭包捕获的变量并转换为 Cell。

---

## P1 — 严重

### 3. 缓存函数内嵌 lambda → foldl/map/filter 原语不工作

~~**症状**：跨表达式定义并调用含 `foldl` + 内嵌 lambda 的缓存函数时，结果不正确。~~

**修复**：`3cb9a33` — `cache_module`/`cache_define` 现在会检查函数体中的变量引用是否能正确 lowering。自递归函数跳过 IR 缓存。
  - `(define (test lst) (foldl (lambda (acc x) (+ acc 1)) 0 lst)) (test (list "a" "b"))` → `2` ✅

### 4. `cadr`/`caddr`/`cadddr` 不存在

**症状**：标准 Scheme composable accessors 未提供。
```scheme
(cadr (list 1 2 3))  → unbound variable
```
**替代**：`(car (cdr lst))`，`(car (cdr (cdr lst)))`。
**根因**：CxR 模式（car/cdr 组合）有原语（`caar`, `cadr`, `cdar`, `cddr` 到 4 层），但标准库中未暴露。

### 5. 错误信息不够详细

**症状**：`"unbound variable: n"` 对 LLM 不够友好。已有行号列号 + `did you mean` 改进。

---

## P2 — 功能缺口

### 6. `hash-has-key?` 不存在

**症状**：需要检查 key 是否存在时只能用 `(hash-ref h key #f)` + 判等。
**替代**：目前没有等效内置原语。

### 7. `set-code` 内容截断

**症状**：LLM 代码超过缓冲区时截断。auto-retry 将 max_tokens 翻倍到 16000。

### 8. arity 检查在 eval() 路径中临时禁用

**原因**：`resolve_callee` 在 arity 检查中跳过 `Primitive` 指令，导致缓存函数调用内部 lambda 时产生误报。

### 9. 标准库导出不完整

`string.aura` 部分函数使用了未导出的依赖。

### 10. 桥接器 body_source fallback 未完全验证

`02681ac` — `body_source` 已加入 `ClosureBridgeData`，但 fallback 路径缺少测试覆盖。

---

## P3 — 增强

### 11. 深递归栈溢出

```scheme
(define (deep n) (if (= n 0) 0 (deep (- n 1))))
(deep 100000)  → Segmentation fault
```
TCO 只优化了 `if` 尾调（通过 `continue` 循环）。非尾递归（如需要返回给调用者）仍用 C++ 递归。

### 12. stdout 不 flush

`(display "a") (display "b")` 可能连在一起输出（在 pipe 模式下尤其明显）。`newline` 可以 flush。

### 13. 类型检查未完全增量

`typecheck-current` 当前全量遍历。

---

## ✅ 今日已修复（5/19）

| 问题 | 提交 |
|------|------|
| JSON 解析器不支持 `\n` 转义 | `e9b4bbf` |
| EDSL `(current-source)` 原语 AST→source 桥接 | `426e877` |
| 错误格式统一 + suggestion 字段 | `c8e8baf` |
| `wrong_arity` typecheck 测试 | `09e71f0` |
| `type_of` typecheck 测试 | `09e71f0` |
| try/catch IR 指令 | `6d06e67` |
| 标准库 v2 (iter/queue/stack/random) | `4b85e46` |
| 关闭闭包 env capture + IR ID 冲突 | 昨日 |
