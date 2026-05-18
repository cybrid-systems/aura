# Aura 已知问题

更新：2026-05-18 v3 — 今日修复后状态

---

## P0 — 阻塞性

### ~~1. `cache_define` 复杂 Lambda 体不持久化~~ ✅ 已修复

`af04a12` + `37f1361`

### ~~2. `require` 在函数体内不支持~~ ✅ 已修复

`1a9b261` — 在 `cache_define` 中检测函数体是否包含 `require`，如有则跳过 IR 缓存，仅用 tree-walker。

### ~~3. Agent 空响应死循环~~ ✅ 已修复

`e4705a8` — 连续 3 轮空代码块自动结束 + Phase 2 上限 10 轮。

---

## P1 — 严重

### 4. 缓存函数内嵌 lambda → foldl/map/filter 原语不工作

**症状**：跨表达式定义并调用含 `foldl` + 内嵌 lambda 的缓存函数时，结果不正确或返回初始累加器。

```
(define (test lst) (foldl (lambda (acc x) (+ acc 1)) 0 lst))
(test (list "a" "b"))  → 1  (应为 2)
```

**根因**：分析中。`begin` 内一次性执行正常，表明 IR 缓存 + 跨表达式加载路径有问题。

**影响**：缓存函数无法使用 `foldl`/`map`/`filter` 等接受闭包参数的原语。

### 5. 错误信息不够详细

**症状**：`"unbound variable: n"` 对 LLM 不够友好。已有行号列号 + `did you mean`。

### 6. `set-code` 内容截断

**症状**：LLM 代码超过缓冲区时截断。auto-retry 将 max_tokens 翻倍到 16000。

---

## P2 — 功能缺口

### 7. arity 检查在 eval() 路径中临时禁用

**原因**：`resolve_callee` 在 arity 检查中跳过 `Primitive` 指令，导致缓存函数调用内部 lambda 时产生误报。已在 `eval_ir()` 等路径保留。

### 8. 标准库导出不完整

`string.aura` 部分函数使用了未导出的依赖。

### 9. 桥接器 body_source fallback 未完全验证

`02681ac` — `body_source` 已加入 `ClosureBridgeData`，但 fallback 路径缺少测试覆盖。

---

## P3 — 增强

### 10. IR 覆盖不全

try/catch/raise、when/unless、cond/case、宏定义走 tree-walker fallback。

### 11. 类型检查未完全增量

`typecheck-current` 当前全量遍历。

---

## ✅ 今日已修复

| 问题 | 提交 |
|------|------|
| 闭包 env capture（free_vars 未赋值 + IR ID 冲突） | `af04a12` |
| 缓存函数字符串池 + lambda_fid + remap 偏移 | `37f1361` |
| Agent 空响应死循环 + known_issues 更新 | `e4705a8` |
| stdlib 导出 map/for-each/member? | `cdf6cc9` |
| Arity 诊断加 caller 名 | `e5335d1` |
| require 函数体内 + arity eval() 路径禁用 | `1a9b261` |
| 桥接器 body_source fallback | `02681ac` |
