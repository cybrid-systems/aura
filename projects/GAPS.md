# Core Gaps — 项目发现的核心短板

按发现顺序累积。修复后移至 "已修复" 区段。

---

## 待修复

### G2: `load` 原语不存在

- **发现于:** kv-store (2026-05-30)
- **严重度:** 🟠
- **场景:** 无法在 Aura REPL 中 `(load "file.aura")` 加载文件。只有 `load-module`，但语义不同（返回 module，不执行顶层表达式）。
- **当前行为:** `(load "file.aura")` → `error: unbound variable: load`
- **期望行为:** 能加载文件并执行其中的顶层定义
- **绕过方式:** `cat file.aura | ./build/aura`

### G3: `void` prim 不存在（已修一半）

- **发现于:** kv-store (2026-05-30)
- **严重度:** 🟡
- **场景:** 需要构造 void 值用于比较 hash-ref 的缺失返回值
- **修复状态:** `(void)` prim 已添加 🟢。但仍无 `make-void`，二者是同一个。

### G4: `equal?` 对 void 值的比较行为

- **发现于:** kv-store (2026-05-30)
- **严重度:** 🟡
- **场景:** `(equal? (void) (void))` 是否为真？
- **当前猜测:** 应为真（同值比较）

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
- **PR:** 已包含在 kv-store 提交中
- **状态:** 已修复 ✅
