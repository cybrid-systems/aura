# Core Gaps — 项目发现的核心短板

按发现顺序累积。修复后移至 "已修复" 区段。

---

## 待修复

### G2: `void` prim 不存在（已修一半）

- **发现于:** kv-store (2026-05-30)
- **严重度:** 🟡
- **场景:** 需要构造 void 值用于比较 hash-ref 的缺失返回值
- **修复状态:** `(void)` prim 已添加 🟢。

---

## 已修复

### G1: `make-hash-table` 不存在，正确名称是 `hash`

- **发现于:** kv-store (2026-05-30)
- **严重度:** 🟢（文档问题）
- **修复:** 使用 `(hash)` 替代
- **状态:** 已适配 ✅

### G5: `make-void` / `void` prim 缺失

- **发现于:** kv-store (2026-05-30)
- **严重度:** 🟡
- **修复:** `src/compiler/evaluator_impl.cpp` 中注册 `(void)` 原语
- **状态:** 已修复 ✅

### G6: `load` 原语缺失

- **发现于:** kv-store (2026-05-30)
- **严重度:** 🟠
- **场景:** 无法在 REPL 中用 `(load "file.aura")` 加载文件
- **修复:** `src/compiler/evaluator_impl.cpp` 中注册 `(load)` 原语
- **状态:** 已修复 ✅
